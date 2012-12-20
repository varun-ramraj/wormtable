from __future__ import print_function
from __future__ import division 

import os
import tempfile
import unittest
import random

import vcfdb
import _vcfdb

def get_int_column(element_size, num_elements=_vcfdb.NUM_ELEMENTS_VARIABLE):
    """
    Returns an integer column with the specified element size and 
    number of elements.
    """
    name = "int_{0}_{1}".format(element_size, num_elements)
    return _vcfdb.Column(name.encode(), b"", _vcfdb.ELEMENT_TYPE_INT, 
            element_size, num_elements)

def get_float_column(element_size, num_elements):
    """
    Returns a float column with the specified element size and 
    number of elements.
    """
    name = "float_{0}_{1}".format(element_size, num_elements)
    return _vcfdb.Column(name.encode(), b"", _vcfdb.ELEMENT_TYPE_FLOAT, 
            element_size, num_elements)

# This must be wrong, as we're not triggering an overflow with this 
# test code.
def get_int_range(element_size):
    """
    Returns the tuple min, max defining the acceptable bounds for an
    integer of the specified size.
    """
    min_v = -2**(8 * element_size - 1)
    max_v = 2**(8 * element_size - 1) - 1
    return min_v, max_v

class TestParser(unittest.TestCase):
    """
    Superclass of all parser tests. Takes care of allocating a database and
    clearing it up afterwards. Subclasses must define the get_columns 
    method.
    """
    def setUp(self):
        fd, self._db_file = tempfile.mkstemp("-parse-test.db") 
        self._columns = self.get_columns()
        self._database = _vcfdb.BerkeleyDatabase(self._db_file.encode(), 
                self._columns, cache_size=1024)
        self._database.create()
        # We can close the open fd now that db has opened it.
        os.close(fd)
        buffer_size = 64 * 1024
        self._row_buffer = _vcfdb.WriteBuffer(self._database, buffer_size, 1)

    def tearDown(self):
        self._row_buffer.flush()
        self._database.close()
        os.unlink(self._db_file)

class TestElementParsers(TestParser):
    """
    Test the element parsers to ensure they accept and reject 
    values correctly.
    """     
    def get_columns(self):
        self._int_columns = {}
        for j in range(1, 9):
            self._int_columns[j] = get_int_column(j, 1)
        self._float_columns = {4: get_float_column(4, 1)}
        cols = list(self._int_columns.values()) + list(self._float_columns.values()) 
        return cols

    def test_bad_types(self):
        """
        Throw bad types at all the columns and expect a type error.
        """
        f = self._row_buffer.insert_encoded_elements
        values = [None, [], {}, self]
        for c in self._columns:
            for v in values:
                self.assertRaises(TypeError, f, c, v)

    def test_good_integer_values(self):
        rb = self._row_buffer
        values = ["\t-1 ", "   -2  ", "0   ", "\n4\n\n", " 14 ", "100"]
        for c in self._int_columns.values():
            for v in values:
                self.assertIsNone(rb.insert_elements(c, int(v)))
                self.assertIsNone(rb.insert_encoded_elements(c, v.encode()))
        # try some values inside of the acceptable range.
        for j in range(1, 9):
            min_v, max_v = get_int_range(j)
            c = self._int_columns[j]
            for k in range(10):
                v = random.randint(min_v, max_v)
                b = str(v).encode()
                self.assertIsNone(rb.insert_elements(c, v))
                self.assertIsNone(rb.insert_encoded_elements(c, b))
             
    def test_bad_integer_values(self):
        rb = self._row_buffer
        values = ["", "--1", "0.25", "sdasd", "[]", "3qsd"]
        for c in self._int_columns.values():
            for v in values:
                e = v.encode()
                self.assertRaises(ValueError, rb.insert_encoded_elements, c, e)
         # try some values outside of the acceptable range.
        for j in range(1, 9):
            min_v, max_v = get_int_range(j)
            c = self._int_columns[j]
            b = str(min_v - 1).encode()
            self.assertRaises(ValueError, rb.insert_encoded_elements, c, b)
            b = str(max_v + 1).encode()
            self.assertRaises(ValueError, rb.insert_encoded_elements, c, b)
            for k in range(10):
                v = random.randint(2 * min_v, min_v - 1)
                b = str(v).encode()
                self.assertRaises(ValueError, rb.insert_encoded_elements, c, b)
                v = random.randint(max_v, 2 * max_v - 1)
                b = str(v).encode()
                self.assertRaises(ValueError, rb.insert_encoded_elements, c, b)

    def test_good_float_values(self):
        rb = self._row_buffer
        values = ["-1", "-2", "0", "4", "14", "100",
            "0.01", "-5.224234345235", "1E12"]
        for c in self._float_columns.values():
            for v in values:
                self.assertIsNone(rb.insert_encoded_elements(c, v.encode()))
                self.assertIsNone(rb.insert_elements(c, float(v)))
            for k in range(10):
                v = random.uniform(-100, 100)
                b = str(v).encode()
                self.assertIsNone(rb.insert_encoded_elements(c, b))
                self.assertIsNone(rb.insert_elements(c, v))
     
    def test_bad_float_values(self):
        rb = self._row_buffer
        values = ["", "--1", "sdasd", "[]", "3qsd", "1Q0.023"]
        for c in self._float_columns.values():
            for v in values:
                e = v.encode()
                self.assertRaises(ValueError, rb.insert_encoded_elements, c, e)
        values = [None, [], 234, "1.23"]
        for c in self._float_columns.values():
            for v in values:
                self.assertRaises(TypeError, rb.insert_elements, c, v)
        

class TestListParsers(TestParser):
    """
    Test the list parsers to make sure that malformed lists are correctly
    detected.
    """     
    def get_columns(self): 
        self._int_columns = {}
        self._float_columns = {}
        for j in range(1, 5):
            self._int_columns[j] = get_int_column(1, j)
            self._float_columns[j] = get_float_column(4, j)
        cols = list(self._int_columns.values()) + list(self._float_columns.values()) 
        return cols
     
    def test_malformed_string_lists(self):
        rb = self._row_buffer
        i1 = self._int_columns[1]
        f1 = self._int_columns[1]
        for s in [b";", b"-", b",", b",;,;"]:
            self.assertRaises(ValueError, rb.insert_encoded_elements, f1, s)
            self.assertRaises(ValueError, rb.insert_encoded_elements, i1, s)
        i2 = self._int_columns[2]
        f2 = self._int_columns[2]
        for s in [b";;", b"-,123", b"0.1,", b"1;2;09"]:
            self.assertRaises(ValueError, rb.insert_encoded_elements, f2, s)
            self.assertRaises(ValueError, rb.insert_encoded_elements, i2, s)

    def test_malformed_python_lists(self):
        rb = self._row_buffer
        i2 = self._int_columns[2]
        f2 = self._int_columns[2]
        for s in [[1], [1, 2, 3], (1, 2, 3), range(40)]:
            self.assertRaises(ValueError, rb.insert_elements, f2, s)
            self.assertRaises(ValueError, rb.insert_elements, i2, s)



    def test_good_integer_values(self):
        rb = self._row_buffer
        values = ["\t-1 ", "   -2  ", "0   ", "\n4\n\n", " 14 ", "100"]
        for n, c in self._int_columns.items(): 
            for j in range(10):
                s = ""
                for r in random.sample(values, n):
                    s += r + ";"
                self.assertIsNone(rb.insert_encoded_elements(c, s.encode()))

    def test_good_float_values(self):
        rb = self._row_buffer
        values = ["\t-1 ", "0.22", " \n1e-7  ", "\n4.0\n\n", " 14 ", "100"]
        for n, c in self._float_columns.items(): 
            for j in range(10):
                s = ""
                for r in random.sample(values, n):
                    s += r + ";"
                self.assertIsNone(rb.insert_encoded_elements(c, s.encode()))







class TestDatabaseLimits(unittest.TestCase):
    """
    Tests the limits of various aspects of the database to see if errors
    are flagged correctly.
    """     
    def setUp(self):
        fd, self._db_file = tempfile.mkstemp("-parse-test.db") 
        n = _vcfdb.MAX_ROW_SIZE // (_vcfdb.MAX_NUM_ELEMENTS * 8 
                + _vcfdb.NUM_ELEMENTS_VARIABLE_OVERHEAD)
        self._columns = [get_int_column(8) for j in range(n + 1)]
        self._database = _vcfdb.BerkeleyDatabase(self._db_file.encode(), 
            self._columns, cache_size=1024)
        self._database.create()
        # We can close the open fd now that db has opened it.
        os.close(fd)
        buffer_size = 64 * 1024
        self._row_buffer = _vcfdb.WriteBuffer(self._database, buffer_size, 1)

    def tearDown(self):
        self._row_buffer.flush()
        self._database.close()
        os.unlink(self._db_file)
    
    def test_column_overflow(self):
        """
        If we set the maximum number of elements to n - 1 columns, 
        it should overflow whatever we do.
        """
        rb = self._row_buffer
        v = [j for j in range(_vcfdb.MAX_NUM_ELEMENTS)]
        n = len(self._columns)
        for k in range(n - 1):
            rb.insert_elements(self._columns[k], v) 
        self.assertRaises(ValueError, rb.insert_elements,
                self._columns[0], v)  
        self.assertRaises(ValueError, rb.insert_elements,
                self._columns[0], v)
        rb.commit_row()
        

    def test_column_allocation_limits(self):
        # Test zero size columns
        self.assertRaises(ValueError, get_int_column, 0, 1)
        self.assertRaises(ValueError, get_float_column, 0, 1)
        
        # Test negative size columns
        self.assertRaises(ValueError, get_int_column, -1, 1)
        self.assertRaises(ValueError, get_float_column, -100, 1)

        # Test negative num_elements 
        self.assertRaises(ValueError, get_int_column, 1, -1)
        self.assertRaises(ValueError, get_float_column, 1, -100)

        
        
