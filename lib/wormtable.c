#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "wormtable.h"

/* 
 * Returns a new string with the specified prefix and suffix 
 * concatenated together and copied to the new buffer.
 */
static char * 
strconcat(const char *prefix, const char *suffix)
{
    char *ret = NULL;
    size_t length = strlen(prefix);
    ret = malloc(1 + length + strlen(suffix));
    if (ret == NULL) {
        goto out; 
    }
    strncpy(ret, prefix, length);
    strcpy(ret + length, suffix);
out:
    return ret;
}

/*
 * Returns a new copy of the specified string.
 */
static char *
copy_string(const char *string)
{
    size_t length = strlen(string);
    char *ret = malloc(1 + length);
    if (ret == NULL) {
        goto out;
    }
    strcpy(ret, string);
out:
    return ret;
}

#ifndef WORDS_BIGENDIAN
/* 
 * Copies n bytes of source into destination, swapping the order of the 
 * bytes.
 */
static void
byteswap_copy(void* dest, void *source, size_t n)
{
    size_t j = 0;
    unsigned char *dest_c = (unsigned char *) dest;
    unsigned char *source_c = (unsigned char *) source;
    for (j = 0; j < n; j++) {
        dest_c[j] = source_c[n - j - 1];
    }
}
#endif

static const char *element_type_strings[] = { "uint", "int", "float", "char" };

static int 
element_type_to_str(u_int32_t element_type, const char **type_string)
{
    int ret = -1;
    if (element_type < sizeof(element_type_strings) / sizeof(char *)) {
        *type_string = element_type_strings[element_type];
        ret = 0;
    }
    return ret;
}

static int 
str_to_element_type(const char *type_string, u_int32_t *element_type)
{
    int ret = -1;
    u_int32_t num_types = sizeof(element_type_strings) / sizeof(char *);
    u_int32_t j;
    for (j = 0; j < num_types; j++) {
        if (strcmp(type_string, element_type_strings[j]) == 0) {
            *element_type = j; 
            ret = 0;
        }
    }
    return ret;
}

static int 
pack_uint(void *dest, u_int64_t value, u_int8_t size) 
{
    int ret = 0;
    void *src = &value;
#ifdef WORDS_BIGENDIAN
    memcpy(dest, src + 8 - size, size);
#else
    byteswap_copy(dest, src, size);
#endif
    printf("pack uint %lu\n", value);
    return ret;
}

static int 
pack_int(void *dest, int64_t value, u_int8_t size) 
{
    int ret = 0;
    int64_t u = value; 
    void *src = &u;
    /* flip the sign bit */
    u ^= 1LL << (size * 8 - 1);
#ifdef WORDS_BIGENDIAN
    memcpy(dest, src + 8 - size, size);
#else
    byteswap_copy(dest, src, size);
#endif
    printf("pack int %ld\n", value);
    return ret;
}

static int 
pack_float4(void *dest, double value, u_int8_t size) 
{
    int ret = 0;
    int32_t bits;
    memcpy(&bits, &value, sizeof(float));
    bits ^= (bits < 0) ? 0xffffffff: 0x80000000;
#ifdef WORDS_BIGENDIAN
    memcpy(dest, &bits, sizeof(float)); 
#else    
    byteswap_copy(dest, &bits, sizeof(float)); 
#endif
    printf("pack float 4 %f\n", value);
    return ret;
}

static int 
pack_float8(void *dest, double value, u_int8_t size) 
{
    int ret = 0;
    int64_t bits;
    memcpy(&bits, &value, sizeof(double));
    bits ^= (bits < 0) ? 0xffffffffffffffffLL: 0x8000000000000000LL;
#ifdef WORDS_BIGENDIAN
    memcpy(dest, &bits, sizeof(double)); 
#else
    byteswap_copy(dest, &bits, sizeof(double)); 
#endif
    return ret;
}


static int
wt_column_pack_elements_uint(wt_column_t *self, void *dest, void *elements,
        u_int32_t num_elements)
{
    int ret = 0;
    void *p = dest;
    u_int32_t j;
    u_int64_t *e = (u_int64_t *) elements;
    for (j = 0; j < num_elements; j++) {
        ret = pack_uint(p, e[j], self->element_size);
        p += sizeof(u_int64_t);
    }
    return ret;
}

static int
wt_column_pack_elements_int(wt_column_t *self, void *dest, void *elements,
        u_int32_t num_elements)
{
    int ret = 0;
    void *p = dest;
    u_int32_t j;
    int64_t *e = (int64_t *) elements;
    for (j = 0; j < num_elements; j++) {
        ret = pack_int(p, e[j], self->element_size);
        p += sizeof(int64_t);
    }
    return ret;
}

static int
wt_column_pack_elements_float4(wt_column_t *self, void *dest, void *elements,
        u_int32_t num_elements)
{
    int ret = 0;
    void *p = dest;
    u_int32_t j;
    double *e = (double *) elements;
    for (j = 0; j < num_elements; j++) {
        ret = pack_float4(p, e[j], self->element_size);
        p += sizeof(double);
    }
    return ret;
}

static int
wt_column_pack_elements_float8(wt_column_t *self, void *dest, void *elements,
        u_int32_t num_elements)
{
    int ret = 0;
    void *p = dest;
    u_int32_t j;
    double *e = (double *) elements;
    for (j = 0; j < num_elements; j++) {
        ret = pack_float8(p, e[j], self->element_size);
        p += sizeof(double);
    }
    return ret;
}



static int
wt_column_pack_elements_char(wt_column_t *self, void *dest, void *elements,
        u_int32_t num_elements)
{
    int ret = 0;
    memcpy(dest, elements, num_elements);
    return ret;
}




/* 
 * Packs the address for a variable column for the specified number of elements
 * at the end of this row, and increase the row size accordingly.
 */
static int 
wt_row_pack_address(wt_row_t *self, wt_column_t *col, u_int32_t num_elements)
{
    int ret = 0;
    void *p = self->data + col->fixed_region_offset;
    u_int32_t new_size = self->size + num_elements * col->element_size;
    if (new_size > WT_MAX_ROW_SIZE) {
        ret = EINVAL;
        goto out;
    }
    ret = pack_uint(p, (u_int64_t) self->size, WT_VARIABLE_OFFSET_SIZE);
    if (ret != 0) {
        goto out;
    }
    p += WT_VARIABLE_OFFSET_SIZE;
    ret = pack_uint(p, (u_int64_t) num_elements, WT_VARIABLE_COUNT_SIZE);
    if (ret != 0) {
        goto out;
    }
    self->size = new_size;
out:
    return ret;
}

static int 
wt_row_set_value(wt_row_t *self, wt_column_t *col, void *elements, 
        u_int32_t num_elements)
{
    int ret = 0;
    void *p;
    if (col->num_elements == WT_VARIABLE) {
        p = self->data + self->size;
        ret = wt_row_pack_address(self, col, num_elements);
        if (ret != 0) {
            goto out;
        }   
    } else {
        p = self->data + col->fixed_region_offset;
        if (num_elements != col->num_elements) {
            ret = EINVAL;
            goto out;
        }
    }
    col->pack_elements(col, p, elements, num_elements);

    printf("Setting elements for column '%s'@%p\n", col->name, p);
out:
    return ret;
}

static int 
wt_row_clear(wt_row_t *self)
{
    memset(self->data, 0, self->size);
    self->size = self->fixed_region_size;
    return 0;
}


static int
wt_table_add_column(wt_table_t *self, const char *name, 
    const char *description, u_int32_t element_type, u_int32_t element_size,
    u_int32_t num_elements)
{
    int ret = 0;
    wt_column_t *col, *last_col;
    /* make some space for the new column */
    self->num_columns++;
    col = realloc(self->columns, self->num_columns * sizeof(wt_column_t));
    if (col == NULL) {
        ret = ENOMEM;
        goto out;
    }
    self->columns = col;
    /* Now fill it in */
    col = &self->columns[self->num_columns - 1];
    col->name = copy_string(name);
    col->description = copy_string(description);
    if (col->name == NULL || col->description == NULL) {
        ret = ENOMEM;
        goto out;
    }
    col->element_type = element_type;
    col->element_size = element_size;
    col->num_elements = num_elements;
    if (element_type == WT_UINT) {
        col->pack_elements = wt_column_pack_elements_uint;
    } else if (element_type == WT_INT) {
        col->pack_elements = wt_column_pack_elements_int;
    } else if (element_type == WT_FLOAT) {
        if (col->element_size == 4) {
            col->pack_elements = wt_column_pack_elements_float4;   
        } else {
            col->pack_elements = wt_column_pack_elements_float8;   
        }
    } else if (element_type == WT_CHAR) {
        col->pack_elements = wt_column_pack_elements_char;
    } else {
        ret = EINVAL;
        goto out;
    }
    col->fixed_region_size = col->num_elements * col->element_size;
    if (col->num_elements == WT_VARIABLE) {
        col->fixed_region_size = WT_VARIABLE_OFFSET_SIZE 
            + WT_VARIABLE_COUNT_SIZE;
    }
    if (self->num_columns == 1) {
        col->fixed_region_offset = 0;
    } else {
        last_col = &self->columns[self->num_columns - 2];
        col->fixed_region_offset = last_col->fixed_region_offset 
                + last_col->fixed_region_size;
    }
    printf("adding column: '%s' :%d %d %d\n", name,
            element_type, element_size, num_elements);
    printf("offset = %d, size = %d\n", col->fixed_region_offset,
            col->fixed_region_size);
out:
    return ret;
}

static int
wt_table_add_column_write_mode(wt_table_t *self, const char *name, 
    const char *description, u_int32_t element_type, u_int32_t element_size,
    u_int32_t num_elements)
{
    int ret = 0;
    if (self->mode != WT_WRITE) {
        ret = EINVAL;
        goto out;
    }
    ret = wt_table_add_column(self, name, description, element_type, 
            element_size, num_elements);
out:
    return ret;
}
    
static int
wt_table_open_writer(wt_table_t *self)
{
    int ret = 0;
    char *db_filename = strconcat(self->homedir, WT_BUILD_PRIMARY_DB_FILE); 
    if (db_filename == NULL) {
        ret = ENOMEM;
        goto out;
    }
    printf("opening table %s for writing\n", self->homedir);   
    self->mode = WT_WRITE;
    ret = mkdir(self->homedir, S_IRWXU);
    if (ret == -1) {
        ret = errno;
        errno = 0;
        goto out;
    }
    ret = self->db->open(self->db, NULL, db_filename, NULL, 
            DB_BTREE, DB_CREATE|DB_EXCL, 0);
    if (ret != 0) {
        goto out;
    }
    self->num_rows = 0;
    /* add the key column */
    wt_table_add_column(self, "row_id", "key column", WT_UINT, self->keysize, 1);

out:
    if (db_filename != NULL) {
        free(db_filename);
    }
    return ret;
}

static int 
wt_table_write_schema(wt_table_t *self)
{
    int ret = 0;
    unsigned int j;
    const char *type_str = NULL;
    wt_column_t *col;
    FILE *f;
    char *schema_file = strconcat(self->homedir, WT_SCHEMA_FILE);
    f = fopen(schema_file, "w");
    if (f == NULL) {
        ret = errno;
        errno = 0;
        goto out;
    }
    fprintf(f, "<?xml version=\"1.0\" ?>\n");
    fprintf(f, "<schema version=\"%s\">\n", WT_SCHEMA_VERSION);
    fprintf(f, "\t<columns>\n");

    for (j = 0; j < self->num_columns; j++) {
        col = &self->columns[j];
        if (element_type_to_str(col->element_type, &type_str)) {
            ret = WT_ERR_FATAL;
        } 
        fprintf(f, "\t\t<column ");
        fprintf(f, "name=\"%s\" ", col->name); 
        fprintf(f, "element_type=\"%s\" ", type_str); 
        fprintf(f, "element_size=\"%d\" ", col->element_size); 
        fprintf(f, "num_elements=\"%d\" ", col->num_elements); 
        fprintf(f, "description =\"%s\" ", col->description); 
        fprintf(f, "/>\n");
    }
    fprintf(f, "\t</columns>\n");
    fprintf(f, "</schema>\n");

out:
    if (f != NULL) {
        if (fclose(f) != 0) {
            ret = errno;
            errno = 0;
        }
    } 
    if (schema_file != NULL) {
        free(schema_file);
    }

    return ret;
}

static int 
wt_table_add_xml_column(wt_table_t *self, xmlNode *node)
{
    int ret = EINVAL;
    xmlAttr *attr;
    xmlChar *xml_name = NULL;
    xmlChar *xml_description = NULL;
    xmlChar *xml_num_elements = NULL;
    xmlChar *xml_element_type = NULL;
    xmlChar *xml_element_size = NULL;
    const char *name, *description;
    u_int32_t element_type;
    int num_elements, element_size;
    for (attr = node->properties; attr != NULL; attr = attr->next) {
        if (xmlStrEqual(attr->name, (const xmlChar *) "name")) {
            xml_name = attr->children->content;
        } else if (xmlStrEqual(attr->name, (const xmlChar *) "description")) {
            xml_description = attr->children->content;
        } else if (xmlStrEqual(attr->name, (const xmlChar *) "num_elements")) {
            xml_num_elements = attr->children->content;
        } else if (xmlStrEqual(attr->name, (const xmlChar *) "element_type")) {
            xml_element_type = attr->children->content;
        } else if (xmlStrEqual(attr->name, (const xmlChar *) "element_size")) {
            xml_element_size = attr->children->content;
        } else {
            goto out;
        }
    }
    if (xml_name == NULL || xml_description == NULL 
            || xml_num_elements == NULL || xml_element_type == NULL
            || xml_element_size == NULL) {
        goto out;
    }
    /* TODO: must do some error checking - atoi is useless. */
    num_elements = atoi((const char *) xml_num_elements);
    element_size = atoi((const char *) xml_element_size);
    ret = str_to_element_type((const char *) xml_element_type, &element_type);
    if (ret != 0) {
        goto out;
    }
    name = (const char *) xml_name;
    description = (const char *) xml_description;
    ret = wt_table_add_column(self, name, description, element_type, 
            element_size, num_elements);
out:
    return ret;
}

static int 
wt_table_read_schema(wt_table_t *self)
{
    int ret = 0;
    const xmlChar *version;
    xmlAttr *attr;
    xmlDocPtr doc = NULL; 
    xmlNode *schema,  *columns, *node;
    char *schema_file = NULL;
    schema_file = strconcat(self->homedir, "schema.xml");
    if (schema_file == NULL) {
        ret = ENOMEM;
        goto out;
    }
    printf("reading schema from %s\n", schema_file);
    doc = xmlReadFile(schema_file, NULL, 0); 
    if (doc == NULL) {
        ret = EINVAL;
        goto out;
    }
    schema = xmlDocGetRootElement(doc);
    if (schema == NULL) {
        printf("parse error");
        ret = EINVAL;
        goto out;
    }
    if (xmlStrcmp(schema->name, (const xmlChar *) "schema")) {
        printf("parse error");
        ret = EINVAL;
        goto out;
    }
    attr = schema->properties;
    while (attr != NULL) {
        //printf("attr:%s = %s\n", attr->name, attr->children->content);
        if (xmlStrEqual(attr->name, (const xmlChar *) "version")) {
            version = attr->children->content;
        }
        attr = attr->next;
    }
    if (version == NULL) {
        printf("parse error: version required");
        ret = EINVAL;
        goto out;
    }
    node = schema->xmlChildrenNode;
    for (node = schema->xmlChildrenNode; node != NULL; node = node->next) {
        if (node->type == XML_ELEMENT_NODE) {
            if (!xmlStrEqual(node->name, (const xmlChar *) "columns")) {
                printf("parse error");
                ret = EINVAL;
                goto out;
            }
            columns = node;
        }
    }
    for (node = columns->xmlChildrenNode; node != NULL; node = node->next) {
        if (node->type == XML_ELEMENT_NODE) {
            if (!xmlStrEqual(node->name, (const xmlChar *) "column")) {
                printf("parse error");
                ret = EINVAL;
                goto out;
            }
            ret = wt_table_add_xml_column(self, node);
            if (ret != 0) {
                goto out;
            }
        }
   }
    
out:
    if (schema_file != NULL) {
        free(schema_file);
    }
    if (doc != NULL) {
        xmlFreeDoc(doc);
    }
    xmlCleanupParser();

    return ret;
}


static int
wt_table_close_writer(wt_table_t *self)
{
    DB *tmp;
    int ret = 0;
    char *old_name = strconcat(self->homedir, WT_BUILD_PRIMARY_DB_FILE);
    char *new_name = strconcat(self->homedir, WT_PRIMARY_DB_FILE);
    if (old_name == NULL || new_name == NULL) {
        ret = ENOMEM;
        goto out;
    }
    ret = wt_table_write_schema(self);
    if (ret != 0) {
        goto out;
    }
    assert(self->db == NULL);
    ret = db_create(&tmp, NULL, 0);
    if (ret != 0) {
        goto out;
    }
    ret = tmp->rename(tmp, old_name, NULL, new_name, 0);
    if (ret != 0) {
        goto out;
    }
    printf("renamed %s to %s\n", old_name, new_name);
out:
    if (old_name != NULL) {
        free(old_name);
    }
    if (new_name != NULL) {
        free(new_name);
    }
    
    return ret;
}


static int
wt_table_open_reader(wt_table_t *self)
{
    int ret;
    char *db_filename = NULL;
    db_filename = strconcat(self->homedir, WT_PRIMARY_DB_FILE);
    if (db_filename == NULL) {
        ret = ENOMEM;
        goto out;
    }
    printf("opening table %s for reading\n", self->homedir);   
    self->mode = WT_READ;
    ret = self->db->open(self->db, NULL, db_filename, NULL, 
            DB_BTREE, DB_RDONLY, 0);
    if (ret != 0) {
        goto out;
    }
    ret = wt_table_read_schema(self);
    if (ret != 0) {
        goto out;
    }
out:
    if (db_filename != NULL) {
        free(db_filename);
    }
    return ret;
}

static int 
wt_table_open(wt_table_t *self, const char *homedir, u_int32_t flags)
{
    int ret = 0;
    self->homedir = homedir;
    if (flags == WT_WRITE) {
        ret = wt_table_open_writer(self); 
    } else if (flags == WT_READ) {
        ret = wt_table_open_reader(self); 
    } else {
        ret = EINVAL; 
    }
    return ret;
}

/* TODO these functions should check the state to make sure we 
 * are in the just-opened state. Check the DB source code to 
 * see how they manage this. We should have a simple state 
 * machine for the table.
 */

static int 
wt_table_set_cachesize(wt_table_t *self, u_int64_t bytes)
{
    int ret = 0;
    u_int64_t gb = 1ULL << 30;
    ret = self->db->set_cachesize(self->db, bytes / gb, bytes % gb, 1);
    return ret;
}

static int 
wt_table_set_keysize(wt_table_t *self, u_int32_t keysize)
{
    int ret = 0;
    if (keysize < 1 || keysize > 8) {
        ret = EINVAL;
        goto out;
    }
    self->keysize = keysize;
out:
    return ret;
}

static int
wt_table_get_column(wt_table_t *self, u_int32_t column_id, wt_column_t **col) 
{
    int ret = -EINVAL;
    if (column_id < self->num_columns) {
        *col = &self->columns[column_id];
    }

    return ret;
}
static void
wt_table_free(wt_table_t *self)
{
    unsigned int j;
    wt_column_t *col;
    if (self->db != NULL) {
        self->db->close(self->db, 0);
    }
    if (self->columns != NULL) {
        for (j = 0; j < self->num_columns; j++) {
            col = &self->columns[j];
            if (col->name != NULL) {
                free(col->name);
            }
            if (col->description != NULL) {
                free(col->description);
            }
        }
        
        free(self->columns);
    }
    free(self);
}



static int 
wt_table_close(wt_table_t *self)
{
    int ret = 0;
   
    ret = self->db->close(self->db, 0);
    self->db = NULL;
    printf("closing table %s\n", self->homedir);   
    if (self->mode == WT_WRITE) {
        ret = wt_table_close_writer(self);
    }
    
    wt_table_free(self);
     
    return ret;
}

static int 
wt_table_alloc_row(wt_table_t *self, wt_row_t **row)
{
    int ret = 0;
    wt_column_t *last_col;
    void *p = malloc(WT_MAX_ROW_SIZE);
    wt_row_t *r = malloc(sizeof(wt_row_t));
    if (r == NULL || p == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    last_col = &self->columns[self->num_columns - 1];
    memset(r, 0, sizeof(wt_row_t));
    r->fixed_region_size = last_col->fixed_region_offset 
            + last_col->fixed_region_size;
    r->size = r->fixed_region_size; 
    r->data = p;
    r->set_value = wt_row_set_value;
    r->clear = wt_row_clear;
    *row = r;
out:
    return ret;
}

static int 
wt_table_free_row(wt_table_t *self, wt_row_t *row)
{
    int ret = 0;
    free(row->data);
    free(row);
    return ret;
}

static int 
wt_table_add_row(wt_table_t *self, wt_row_t *row)
{
    int ret = 0;
    wt_column_t *id_col = &self->columns[0];
    DBT key, data;
    ret = row->set_value(row, id_col, &self->num_rows, 1);
    if (ret != 0) {
        goto out;
    }
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));
    key.data = row->data;
    key.size = self->keysize;
    data.data = row->data + self->keysize;
    data.size = row->size - self->keysize;
    
    printf("data size = %d\n", data.size);
    ret = self->db->put(self->db, NULL, &key, &data, 0);
    if (ret != 0) {
        goto out;
    }
    self->num_rows++;
    printf("added row\n");
out:
    return ret;
}


int 
wt_table_create(wt_table_t **wtp)
{
    int ret = 0;
    wt_table_t *self = malloc(sizeof(wt_table_t));
    if (self == NULL) {
        ret = ENOMEM;
        goto out;
    }
    memset(self, 0, sizeof(wt_table_t));
    ret = db_create(&self->db, NULL, 0);
    if (ret != 0) {
        goto out;
    }
    self->keysize = WT_DEFAULT_KEYSIZE;
    self->open = wt_table_open;
    self->add_column = wt_table_add_column_write_mode;
    self->set_cachesize = wt_table_set_cachesize;
    self->set_keysize = wt_table_set_keysize;
    self->get_column = wt_table_get_column;
    self->close = wt_table_close;
    self->alloc_row = wt_table_alloc_row;
    self->free_row = wt_table_free_row;
    self->add_row = wt_table_add_row;
    *wtp = self;
out:
    if (ret != 0) {
        wt_table_free(self);
    }

    return ret;
}

char *
wt_strerror(int err)
{
    return db_strerror(err);
}

