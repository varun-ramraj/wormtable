#include <Python.h>
#include <structmember.h>

#include <db.h>


#if PY_MAJOR_VERSION >= 3
#define IS_PY3K
#endif

#define ELEMENT_TYPE_CHAR 0
#define ELEMENT_TYPE_INT 1 
#define ELEMENT_TYPE_FLOAT 2
#define ELEMENT_TYPE_ENUM 3

#define NUM_ELEMENTS_VARIABLE 0 
#define MAX_ELEMENTS 256
#define MAX_RECORD_SIZE 65536

#define MODULE_DOC \
"Low level Berkeley DB interface for vcfdb"

static PyObject *BerkeleyDatabaseError;

typedef struct Column_t {
    PyObject_HEAD
    PyObject *name;
    PyObject *description;
    PyObject *enum_values;
    int element_type;
    int element_size;
    int num_elements;
    int fixed_region_offset;
    /* possibly better names for this... */
    int (*convert_string)(struct Column_t*, char *, void *);
} Column;


typedef struct {
    PyObject_HEAD
    DB *primary_db;
    PyBytesObject *filename;
    PyListObject *columns;
    Py_ssize_t cache_size;
    int fixed_region_size;
    int key_size;
} BerkeleyDatabase;

typedef struct {
    PyObject_HEAD
    BerkeleyDatabase *database;
    /* arrays for double buffering */
    void *key_buffer;
    void *data_buffer;
    int num_records;
    int current_data_offset;
    int current_key_offset;
    int current_record_size;
    DBT *key_dbts; 
    DBT *data_dbts; 
    /* end of arrays */
    int max_num_records;
    int data_buffer_size;
    int key_buffer_size;
    u_int64_t record_id; 
    /*
    u_int32_t record_buffer_size;
    int current_record_size;
    u_int32_t current_record_offset;
    unsigned char *key_buffer;
    u_int32_t key_size;
    u_int32_t num_records;
    u_int32_t max_num_records;
    DBT *keys; 
    DBT *records; 
    */
} WriteBuffer;

static void 
handle_bdb_error(int err)
{
    PyErr_SetString(BerkeleyDatabaseError, db_strerror(err));
}

/* 
 * Copies n bytes of source into destination, swapping the order of the 
 * bytes if necessary.
 *
 * TODO We need to put in #defs to detect endianness and adapt accordingly.
 * Currently this is little-endian only.
 */
static void
bigendian_copy(void* dest, void *source, size_t n)
{
    size_t j = 0;
    unsigned char *dest_c = (unsigned char *) dest;
    unsigned char *source_c = (unsigned char *) source;
    for (j = 0; j < n; j++) {
        dest_c[j] = source_c[n - j - 1];
    }
    /*
    for (j = 0; j < n; j++) {
        printf("%03d ", source_c[j]); 
    }
    printf("\t -> \t"); 
    for (j = 0; j < n; j++) {
        printf("%03d ", dest_c[j]); 
    }
    printf("\n");
    */
}




/*==========================================================
 * Column object 
 *==========================================================
 */


static int 
Column_convert_string_int(Column *self, char *string, void *destination)
{
    int ret = -1;
    char *v = string;
    char *tail;
    long long element;
    int num_elements = 1;
    int not_done = 1; 
    void *dest = destination;
    /* TODO check this - probably not totally right */
    long long max_element_value = (1l << (8 * self->element_size - 1)) - 1;
    while (not_done) {
        if (*v == ',') {
            v++;
            num_elements++;
            if (num_elements >= MAX_ELEMENTS) {
                PyErr_SetString(PyExc_ValueError, "Too many elements");
                goto out;
            }
        }
        if (*v == 0) {
            not_done = 0; 
        } else {
            errno = 0;
            element = strtoll(v, &tail, 0);
            if (errno) {
                PyErr_SetString(PyExc_ValueError, "Element overflow");
                goto out;
            }
            if (abs(element) >= max_element_value) {
                PyErr_SetString(PyExc_ValueError, "Value too large");
                goto out;
            }
            
            bigendian_copy(dest, &element, self->element_size);
            dest += self->element_size;
            if (v == tail) {
                PyErr_SetString(PyExc_ValueError, "Element parse error");
                goto out;
            }
            v = tail;
        }
    }
    if (self->num_elements != NUM_ELEMENTS_VARIABLE) {
        if (num_elements != self->num_elements) {
            PyErr_SetString(PyExc_ValueError, "incorrect number of elements");
            goto out;
        }
    }
        
    ret = num_elements;
out:
    
    return ret;
}

static int 
Column_convert_string_float(Column *self, char *string, void *destination)
{
    int ret = -1;
    char *v = string;
    char *tail;
    float element;
    int num_elements = 1;
    int not_done = 1; 
    void *dest = destination;
    while (not_done) {
        if (*v == ',') {
            v++;
            num_elements++;
            if (num_elements >= MAX_ELEMENTS) {
                PyErr_SetString(PyExc_ValueError, "Too many elements");
                goto out;
            }
        }
        if (*v == 0) {
            not_done = 0; 
        } else {
            errno = 0;
            element = (float) strtod(v, &tail);
            if (errno) {
                PyErr_SetString(PyExc_ValueError, "Element overflow");
                goto out;
            }
            bigendian_copy(dest, &element, self->element_size);
            dest += self->element_size;
            if (v == tail) {
                PyErr_SetString(PyExc_ValueError, "Element parse error");
                goto out;
            }
            v = tail;
        }
    }
    if (self->num_elements != NUM_ELEMENTS_VARIABLE) {
        if (num_elements != self->num_elements) {
            PyErr_SetString(PyExc_ValueError, "incorrect number of elements");
            goto out;
        }
    }
    ret = num_elements;
out:
    
    return ret;
}
 
static int 
Column_convert_string_char(Column *self, char *string, void *destination)
{
    //printf("Convert char %d:%s\n", self->num_elements, string);    
    size_t n = strlen(string);
    memcpy(destination, string, n); 
    return n;
}

/*
 * TODO We need to support lists of enumeration values - this will need to 
 * copy the parsing code above.
 */
static int 
Column_convert_string_enum(Column *self, char *string, void *destination)
{
    int ret = -1;
    unsigned long value; 
    unsigned long max_value = 1l << (8 * self->element_size);
    PyObject *v = PyDict_GetItemString(self->enum_values, string);
    if (v == NULL) {
        value = PyDict_Size(self->enum_values) + 1;
        if (value > max_value) {
            PyErr_SetString(PyExc_ValueError, "Enum value too large");
            goto out;
        }
        v = PyLong_FromUnsignedLong(value);
        if (v == NULL) {
            PyErr_NoMemory();
            goto out; 
        }
        if (PyDict_SetItemString(self->enum_values, string, v) < 0) {
            Py_DECREF(v); 
            goto out;
        }
        Py_DECREF(v);    
    } else {
        if (!PyLong_Check(v)) {
            PyErr_SetString(PyExc_ValueError, "Enum value not a long");
            goto out;
        }
        value = PyLong_AsUnsignedLong(v); 
    }
    bigendian_copy(destination, &value, self->element_size);
    //printf("%s -> %ld\n", string, value);
    ret = 1;
out:
    return ret;
}

/* 
 * Returns the number of bytes that this column occupies in the 
 * fixed region of records.
 */  
static int 
Column_get_fixed_region_size(Column *self) 
{
    int ret = self->element_size * self->num_elements;
    if (self->num_elements == NUM_ELEMENTS_VARIABLE) {
        /* two byte offset + one byte count */
        ret = 3;
    }
    return ret;
}

static void
Column_dealloc(Column* self)
{
    Py_XDECREF(self->name); 
    Py_XDECREF(self->description); 
    Py_XDECREF(self->enum_values); 
    Py_TYPE(self)->tp_free((PyObject*)self);
}


static int
Column_init(Column *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    static char *kwlist[] = {"name", "description",  "element_type", 
        "element_size", "num_elements", NULL};
    PyObject *name = NULL;
    PyObject *description = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOiii", kwlist, 
            &name, &description, 
            &self->element_type, &self->element_size, 
            &self->num_elements)) {  
        goto out;
    }
    self->name = name;
    self->description = description;
    Py_INCREF(self->name);
    Py_INCREF(self->description);
    if (self->element_type == ELEMENT_TYPE_INT) {
        if (self->element_size < 1 || self->element_size > 8) {
            PyErr_SetString(PyExc_ValueError, "bad element size");
            goto out;
        }
        self->convert_string = Column_convert_string_int;
    } else if (self->element_type == ELEMENT_TYPE_FLOAT) {
        if (self->element_size != sizeof(float)) {
            PyErr_SetString(PyExc_ValueError, "bad element size");
            goto out;
        }
        self->convert_string = Column_convert_string_float;
    } else if (self->element_type == ELEMENT_TYPE_CHAR) {
        if (self->element_size != 1) {
            PyErr_SetString(PyExc_ValueError, "bad element size");
            goto out;
        }
        self->convert_string = Column_convert_string_char;
    } else if (self->element_type == ELEMENT_TYPE_ENUM) {
        if (self->element_size < 1 || self->element_size > 2) {
            PyErr_SetString(PyExc_ValueError, "bad element size");
            goto out;
        }
        self->convert_string = Column_convert_string_enum;
    } else {    
        PyErr_SetString(PyExc_ValueError, "Unknown element type");
        goto out;
    }
    
    self->enum_values = PyDict_New();
    if (self->enum_values == NULL) {
        goto out;
    }
    Py_INCREF(self->enum_values);
    

    ret = 0;
out:
    return ret;
}

static PyMemberDef Column_members[] = {
    {"name", T_OBJECT_EX, offsetof(Column, name), READONLY, "name"},
    {"description", T_OBJECT_EX, offsetof(Column, description), READONLY, "description"},
    {"enum_values", T_OBJECT_EX, offsetof(Column, enum_values), 0, "enum_values"},
    {"element_type", T_INT, offsetof(Column, element_type), READONLY, "element_type"},
    {"element_size", T_INT, offsetof(Column, element_size), READONLY, "element_size"},
    {"num_elements", T_INT, offsetof(Column, num_elements), READONLY, "num_elements"},
    {"fixed_region_offset", T_INT, offsetof(Column, fixed_region_offset), 
        READONLY, "fixed_region_offset"},
    {NULL}  /* Sentinel */
};


static PyMethodDef Column_methods[] = {
    {NULL}  /* Sentinel */
};

static PyTypeObject ColumnType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_vcfdb.Column",             /* tp_name */
    sizeof(Column),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)Column_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT |
        Py_TPFLAGS_BASETYPE,   /* tp_flags */
    "Column objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    Column_methods,             /* tp_methods */
    Column_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Column_init,      /* tp_init */
};
   


/*==========================================================
 * BerkeleyDatabase object 
 *==========================================================
 */

static void
BerkeleyDatabase_dealloc(BerkeleyDatabase* self)
{
    Py_XDECREF(self->filename);
    Py_XDECREF(self->columns);
    /* make sure that the DB handles are closed. We can ignore errors here. */ 
    if (self->primary_db != NULL) {
        self->primary_db->close(self->primary_db, 0); 
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
BerkeleyDatabase_init(BerkeleyDatabase *self, PyObject *args, PyObject *kwds)
{
    DB *db;
    int j;
    Column *col;
    int ret = -1;
    int db_ret = 0;
    u_int32_t gigs, bytes;
    Py_ssize_t gigabyte = 1024 * 1024 * 1024;
    static char *kwlist[] = {"filename", "columns", "cache_size", NULL}; 
    PyBytesObject *filename = NULL;
    PyListObject *columns = NULL;
    self->primary_db = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O!n", kwlist, 
            &PyBytes_Type, &filename, 
            &PyList_Type,  &columns, 
            &self->cache_size)) {
        goto out;
    }
    self->filename = filename;
    self->columns = columns;
    Py_INCREF(self->columns);
    Py_INCREF(self->filename);
    db_ret = db_create(&db, NULL, 0);
    self->primary_db = db;
    if (db_ret != 0) {
        handle_bdb_error(db_ret);
        goto out;    
    }
    gigs = (u_int32_t) (self->cache_size / gigabyte);
    bytes = (u_int32_t) (self->cache_size % gigabyte);
    db_ret = db->set_cachesize(db, gigs, bytes, 1); 
    if (db_ret != 0) {
        handle_bdb_error(db_ret);
        goto out;    
    }
    self->key_size = 5; /* TODO make this part of kwargs */
    /* calculate the variable region offset by summing up the fixed 
     * region size from each column
     */
    for (j = 0; j < PyList_GET_SIZE(self->columns); j++) {
        col = (Column *) PyList_GET_ITEM(self->columns, j);
        if (!PyObject_TypeCheck(col, &ColumnType)) {
            PyErr_SetString(PyExc_ValueError, "Must be Column objects");
            goto out;
        }
        col->fixed_region_offset = self->fixed_region_size;
        self->fixed_region_size += Column_get_fixed_region_size(col);
    }
    /* TODO check to make sure these are within 64K */ 

    ret = 0;
out:

    return ret;
}


static PyMemberDef BerkeleyDatabase_members[] = {
    {"filename", T_OBJECT_EX, offsetof(BerkeleyDatabase, filename), READONLY, "filename"},
    {"columns", T_OBJECT_EX, offsetof(BerkeleyDatabase, columns), READONLY, "columns"},
    {"cache_size", T_PYSSIZET, offsetof(BerkeleyDatabase, cache_size), READONLY, "cache_size"},
    {"fixed_region_size", T_INT, offsetof(BerkeleyDatabase, fixed_region_size), READONLY, "fixed_region_size"},
    {NULL}  /* Sentinel */
};



static PyObject *
BerkeleyDatabase_create(BerkeleyDatabase* self)
{
    PyObject *ret = NULL;
    int db_ret;
    u_int32_t flags = DB_CREATE|DB_TRUNCATE;
    char *db_name = NULL;
    db_name = PyBytes_AsString((PyObject *) self->filename);
    db_ret = self->primary_db->open(self->primary_db, NULL, db_name, NULL, 
            DB_BTREE,  flags,  0);         
    if (db_ret != 0) {
        handle_bdb_error(db_ret);
        goto out;    
    }
    ret = Py_BuildValue("");
out:
    return ret; 
}

static PyObject *
BerkeleyDatabase_close(BerkeleyDatabase* self)
{
    PyObject *ret = NULL;
    int db_ret;
    if (self->primary_db != NULL) {
        db_ret = self->primary_db->close(self->primary_db, 0); 
        if (db_ret != 0) {
            handle_bdb_error(db_ret);
            goto out;    
        }
    }
    ret = Py_BuildValue("");
out:
    self->primary_db = NULL;
    return ret; 
}


static PyMethodDef BerkeleyDatabase_methods[] = {
    {"create", (PyCFunction) BerkeleyDatabase_create, METH_NOARGS, "Create the database" },
    {"close", (PyCFunction) BerkeleyDatabase_close, METH_NOARGS, "Close the database" },
    {NULL}  /* Sentinel */
};


static PyTypeObject BerkeleyDatabaseType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_vcfdb.BerkeleyDatabase",             /* tp_name */
    sizeof(BerkeleyDatabase),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)BerkeleyDatabase_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT |
        Py_TPFLAGS_BASETYPE,   /* tp_flags */
    "BerkeleyDatabase objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    BerkeleyDatabase_methods,             /* tp_methods */
    BerkeleyDatabase_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)BerkeleyDatabase_init,      /* tp_init */
};
   


/*==========================================================
 * WriteBuffer object 
 *==========================================================
 */

static void
WriteBuffer_clear_buffers(WriteBuffer* self)
{
    memset(self->data_buffer, 0, self->data_buffer_size);
    memset(self->key_buffer, 0, self->key_buffer_size);
    self->num_records = 0;
    self->current_data_offset = 0;
    self->current_key_offset = 0;
    self->current_record_size = self->database->fixed_region_size;
}
static void
WriteBuffer_dealloc(WriteBuffer* self)
{
    PyMem_Free(self->data_buffer);
    PyMem_Free(self->key_buffer);
    PyMem_Free(self->data_dbts);
    PyMem_Free(self->key_dbts);
    Py_XDECREF(self->database);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
WriteBuffer_init(WriteBuffer *self, PyObject *args, PyObject *kwds)
{
    int ret = -1; 
    static char *kwlist[] = {"database", "data_buffer_size", 
        "max_num_records", NULL};
    BerkeleyDatabase *database = NULL;
    self->database = NULL;
    self->key_buffer = NULL; 
    self->data_buffer = NULL; 
    self->key_dbts = NULL; 
    self->data_dbts = NULL; 
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!ii", kwlist, 
            &BerkeleyDatabaseType, &database, &self->data_buffer_size,
            &self->max_num_records)) {
        goto out;
    }
    self->database = database;
    Py_INCREF(self->database);
    if (self->data_buffer_size < MAX_RECORD_SIZE) {
        PyErr_SetString(PyExc_ValueError, "data buffer size too small");
        goto out;
    }
    if (self->max_num_records < 1) {
        PyErr_SetString(PyExc_ValueError, "must have >= 1 records in buffer.");
        goto out;
    }
    self->key_buffer_size = self->max_num_records * self->database->key_size;
    /* Alloc some memory */
    self->key_buffer = PyMem_Malloc(self->key_buffer_size);
    if (self->key_buffer == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    self->data_buffer = PyMem_Malloc(self->data_buffer_size);
    if (self->data_buffer == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    self->key_dbts = PyMem_Malloc(self->max_num_records * sizeof(DBT));
    if (self->key_dbts == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    self->data_dbts = PyMem_Malloc(self->max_num_records * sizeof(DBT));
    if (self->data_dbts == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    /* zero the DBTs before use */
    memset(self->key_dbts, 0, self->max_num_records * sizeof(DBT));
    memset(self->data_dbts, 0, self->max_num_records * sizeof(DBT));
    self->record_id = 0;
    WriteBuffer_clear_buffers(self);
    
    ret = 0;
out:
    return ret;
}

static PyMemberDef WriteBuffer_members[] = {
    {"database", T_OBJECT_EX, offsetof(WriteBuffer, database), READONLY, "database"},
    {"max_num_records", T_INT, offsetof(WriteBuffer, max_num_records), READONLY, "max_num_records"},
    {"data_buffer_size", T_INT, offsetof(WriteBuffer, data_buffer_size), READONLY, "data_buffer_size"},
    {"key_buffer_size", T_INT, offsetof(WriteBuffer, key_buffer_size), READONLY, "key_buffer_size"},
    {NULL}  /* Sentinel */
};

static PyObject *
WriteBuffer_flush(WriteBuffer* self)
{
    PyObject *ret = NULL;
    int db_ret = 0;
    u_int32_t j;
    DB *db;
    DBT *key, *data;
    db = self->database->primary_db;
    //printf("Flushing buffer: %d records in %dKiB\n", self->num_records, 
    //        self->current_data_offset / 1024);
    for (j = 0; j < self->num_records; j++) {
        key = &self->key_dbts[j];
        data = &self->data_dbts[j];
        db_ret = db->put(db, NULL, key, data, 0);
        if (db_ret != 0) {
            handle_bdb_error(db_ret); 
            goto out;
        } 
    }
    //printf("done\n");
    WriteBuffer_clear_buffers(self);

    ret = Py_BuildValue("");
out:
    return ret; 
}

static PyObject *
WriteBuffer_set_record_value(WriteBuffer* self, PyObject *args)
{
    PyObject *ret = NULL;
    Column *column = NULL;
    PyBytesObject *value = NULL;
    unsigned int num_elements, offset;
    void *dest = NULL; 
    char *v;
    if (!PyArg_ParseTuple(args, "O!O!", &ColumnType, &column, &PyBytes_Type,
            &value)) {
        goto out;
    }
    Py_INCREF(column);
    Py_INCREF(value);
    v = PyBytes_AsString((PyObject *) value);
    offset = column->fixed_region_offset;
    if (column->num_elements == NUM_ELEMENTS_VARIABLE) {
        offset = self->current_record_size;
    }
    dest = self->data_buffer + self->current_data_offset + offset;
    num_elements = column->convert_string(column, v, dest);
    if (num_elements < 0) {
        goto out;
    }
    if (column->num_elements == NUM_ELEMENTS_VARIABLE) {
        offset = column->fixed_region_offset;
        dest = self->data_buffer + self->current_data_offset + offset;
        bigendian_copy(dest, &offset, 2); // FIXME
        dest += 2;
        bigendian_copy(dest, &num_elements, 1); // FIXME
        self->current_record_size += num_elements * column->element_size;
    }
    ret = Py_BuildValue("");
out:
    Py_XDECREF(column);
    Py_XDECREF(value);
    return ret; 
}



static PyObject *
WriteBuffer_commit_record(WriteBuffer* self, PyObject *args)
{
    DBT *key, *data;
    PyObject *ret = NULL;
    void *dest;
    int barrier = self->data_buffer_size - MAX_RECORD_SIZE;
    dest = self->key_buffer + self->current_key_offset;
    bigendian_copy(dest, &self->record_id, self->database->key_size);
    /*
    int j;
    printf("Commit:%llu %d \n", key_val, self->current_record_size);
    for (j = 0; j < 1024; j++) {
        printf("%03d ", ((unsigned char *) self->data_buffer)[j]);
    }
    printf("\n");
    */
    key = &self->key_dbts[self->num_records];
    data = &self->data_dbts[self->num_records];
    key->size = self->database->key_size;
    key->data = self->key_buffer + self->current_key_offset;
    data->size = self->current_record_size;
    data->data = self->data_buffer + self->current_data_offset; 
    /* We are done with this record, so increment the counters */ 
    self->record_id++;
    self->current_record_size = self->database->fixed_region_size;
    self->current_key_offset += self->database->key_size;
    self->current_data_offset += self->current_record_size;
    self->num_records++;
    if (self->current_data_offset >= barrier 
            || self->num_records == self->max_num_records) {
        ret = WriteBuffer_flush(self);
        if (ret == NULL) {
            goto out;
        }
    }

    ret = Py_BuildValue("");
out:
    return ret; 
}




static PyMethodDef WriteBuffer_methods[] = {
    {"set_record_value", (PyCFunction) WriteBuffer_set_record_value, METH_VARARGS, "set value" },
    {"commit_record", (PyCFunction) WriteBuffer_commit_record, METH_VARARGS, "commit record" },
    {"flush", (PyCFunction) WriteBuffer_flush, METH_NOARGS, "flush" },
    {NULL}  /* Sentinel */
};


static PyTypeObject WriteBufferType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_vcfdb.WriteBuffer",             /* tp_name */
    sizeof(WriteBuffer),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)WriteBuffer_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "WriteBuffer objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    WriteBuffer_methods,             /* tp_methods */
    WriteBuffer_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)WriteBuffer_init,      /* tp_init */
};
 


/* Initialisation code supports Python 2.x and 3.x. The framework uses the 
 * recommended structure from http://docs.python.org/howto/cporting.html. 
 * I've ignored the point about storing state in globals, as the examples 
 * from the Python documentation still use this idiom. 
 */

#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef vcfdbmodule = {
    PyModuleDef_HEAD_INIT,
    "_vcfdb",   /* name of module */
    MODULE_DOC, /* module documentation, may be NULL */
    -1,    
    NULL, NULL, NULL, NULL, NULL 
};

#define INITERROR return NULL

PyObject * 
PyInit__vcfdb(void)

#else
#define INITERROR return

void
init_vcfdb(void)
#endif
{
#if PY_MAJOR_VERSION >= 3
    PyObject *module = PyModule_Create(&vcfdbmodule);
#else
    PyObject *module = Py_InitModule3("_vcfdb", NULL, MODULE_DOC);
#endif
    if (module == NULL) {
        INITERROR;
    }
    /* BerkeleyDatabase */
    BerkeleyDatabaseType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&BerkeleyDatabaseType) < 0) {
        INITERROR;
    }
    Py_INCREF(&BerkeleyDatabaseType);
    PyModule_AddObject(module, "BerkeleyDatabase", 
            (PyObject *) &BerkeleyDatabaseType);
    /* Column */
    ColumnType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&ColumnType) < 0) {
        INITERROR;
    }
    Py_INCREF(&ColumnType);
    PyModule_AddObject(module, "Column", (PyObject *) &ColumnType);
    
    /* WriteBuffer */
    WriteBufferType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&WriteBufferType) < 0) {
        INITERROR;
    }
    Py_INCREF(&WriteBufferType);
    PyModule_AddObject(module, "WriteBuffer", (PyObject *) &WriteBufferType);
    
    BerkeleyDatabaseError = PyErr_NewException("_vcfdb.BerkeleyDatabaseError", 
            NULL, NULL);
    Py_INCREF(BerkeleyDatabaseError);
    PyModule_AddObject(module, "BerkeleyDatabaseError", BerkeleyDatabaseError);
    
    PyModule_AddIntConstant(module, "NUM_ELEMENTS_VARIABLE", 
            NUM_ELEMENTS_VARIABLE);
    PyModule_AddIntConstant(module, "ELEMENT_TYPE_CHAR", ELEMENT_TYPE_CHAR);
    PyModule_AddIntConstant(module, "ELEMENT_TYPE_INT", ELEMENT_TYPE_INT);
    PyModule_AddIntConstant(module, "ELEMENT_TYPE_FLOAT", ELEMENT_TYPE_FLOAT);
    PyModule_AddIntConstant(module, "ELEMENT_TYPE_ENUM", ELEMENT_TYPE_ENUM);

#if PY_MAJOR_VERSION >= 3
    return module;
#endif
}


