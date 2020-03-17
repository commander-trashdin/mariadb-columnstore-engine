/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/*****************************************************************************
 * $Id: column.cpp 2103 2013-06-04 17:53:38Z dcathey $
 *
 ****************************************************************************/
#include <iostream>
#include <sstream>
//#define NDEBUG
#include <cassert>
#include <cmath>
#ifndef _MSC_VER
#include <pthread.h>
#else
#endif
using namespace std;

#include <boost/scoped_array.hpp>
using namespace boost;

#include "branchpred.h"
#include "primitiveprocessor.h"
#include "messagelog.h"
#include "messageobj.h"
#include "we_type.h"
#include "stats.h"
#include "primproc.h"
#include "dataconvert.h"
using namespace logging;
using namespace dbbc;
using namespace primitives;
using namespace primitiveprocessor;
using namespace execplan;

namespace
{
using RID_T = uint16_t;  // Row index type, as used in rid arrays

// Column filtering is dispatched 4-way based on the column type,
// which defines implementation of comparison operations for the column values
enum ENUM_KIND {KIND_DEFAULT,   // compared as signed integers
                KIND_UNSIGNED,  // compared as unsigned integers
                KIND_FLOAT,     // compared as floating-point numbers
                KIND_TEXT};     // whitespace-trimmed and then compared as signed integers


/*****************************************************************************
 *** AUXILIARY FUNCTIONS *****************************************************
 *****************************************************************************/

// File-local event logging helper
void logIt(int mid, int arg1, const char* arg2 = NULL)
{
    MessageLog logger(LoggingID(28));
    logging::Message::Args args;
    Message msg(mid);

    args.add(arg1);

    if (arg2 && *arg2)
        args.add(arg2);

    msg.format(args);
    logger.logErrorMessage(msg);
}

// Reverse the byte order
inline uint64_t order_swap(uint64_t x)
{
    uint64_t ret = (x >> 56) |
                   ((x << 40) & 0x00FF000000000000ULL) |
                   ((x << 24) & 0x0000FF0000000000ULL) |
                   ((x << 8)  & 0x000000FF00000000ULL) |
                   ((x >> 8)  & 0x00000000FF000000ULL) |
                   ((x >> 24) & 0x0000000000FF0000ULL) |
                   ((x >> 40) & 0x000000000000FF00ULL) |
                   (x << 56);
    return ret;
}

// Portable way to copy value
inline void copyValue(void* out, const void *in, size_t size)
{
    memcpy(out, in, size);  //// we are relying on little-endiannes here if actual *in has width >size
}

// char(8) values lose their null terminator
template <int COL_WIDTH>
inline string fixChar(int64_t intval)
{
    char chval[COL_WIDTH + 1];
    memcpy(chval, &intval, COL_WIDTH);
    chval[COL_WIDTH] = '\0';

    return string(chval);
}

//FIXME: what are we trying to accomplish here? It looks like we just want to count
// the chars in a string arg?
inline p_DataValue convertToPDataValue(const void* val, int COL_WIDTH)
{
    p_DataValue dv;
    string str;

    if (8 == COL_WIDTH)
        str = fixChar<8>(*reinterpret_cast<const int64_t*>(val));
    else
        str = reinterpret_cast<const char*>(val);

    dv.len = static_cast<int>(str.length());
    dv.data = reinterpret_cast<const uint8_t*>(val);
    return dv;
}


/*****************************************************************************
 *** NULL/EMPTY VALUES FOR EVERY COLUMN TYPE/WIDTH ***************************
 *****************************************************************************/

// Bit pattern representing EMPTY value for given column type/width
template<int COL_WIDTH>
uint64_t getEmptyValue(uint8_t type);

template<>
uint64_t getEmptyValue<8>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::DOUBLE:
        case CalpontSystemCatalog::UDOUBLE:
            return joblist::DOUBLEEMPTYROW;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
        case CalpontSystemCatalog::VARBINARY:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR8EMPTYROW;

        case CalpontSystemCatalog::UBIGINT:
            return joblist::UBIGINTEMPTYROW;

        default:
            return joblist::BIGINTEMPTYROW;
    }
}

template<>
uint64_t getEmptyValue<4>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::FLOAT:
        case CalpontSystemCatalog::UFLOAT:
            return joblist::FLOATEMPTYROW;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR4EMPTYROW;

        case CalpontSystemCatalog::UINT:
        case CalpontSystemCatalog::UMEDINT:
            return joblist::UINTEMPTYROW;

        default:
            return joblist::INTEMPTYROW;
    }
}

template<>
uint64_t getEmptyValue<2>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR2EMPTYROW;

        case CalpontSystemCatalog::USMALLINT:
            return joblist::USMALLINTEMPTYROW;

        default:
            return joblist::SMALLINTEMPTYROW;
    }
}

template<>
uint64_t getEmptyValue<1>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR1EMPTYROW;

        case CalpontSystemCatalog::UTINYINT:
            return joblist::UTINYINTEMPTYROW;

        default:
            return joblist::TINYINTEMPTYROW;
    }
}


// Bit pattern representing NULL value for given column type/width
template<int COL_WIDTH>
uint64_t getNullValue(uint8_t type);

template<>
uint64_t getNullValue<8>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::DOUBLE:
        case CalpontSystemCatalog::UDOUBLE:
            return joblist::DOUBLENULL;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
        case CalpontSystemCatalog::VARBINARY:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR8NULL;

        case CalpontSystemCatalog::UBIGINT:
            return joblist::UBIGINTNULL;

        default:
            return joblist::BIGINTNULL;
    }
}

template<>
uint64_t getNullValue<4>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::FLOAT:
        case CalpontSystemCatalog::UFLOAT:
            return joblist::FLOATNULL;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR4NULL;

        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::DATENULL;

        case CalpontSystemCatalog::UINT:
        case CalpontSystemCatalog::UMEDINT:
            return joblist::UINTNULL;

        default:
            return joblist::INTNULL;
    }
}

template<>
uint64_t getNullValue<2>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR2NULL;

        case CalpontSystemCatalog::USMALLINT:
            return joblist::USMALLINTNULL;

        default:
            return joblist::SMALLINTNULL;
    }
}

template<>
uint64_t getNullValue<1>(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR1NULL;

        case CalpontSystemCatalog::UTINYINT:
            return joblist::UTINYINTNULL;

        default:
            return joblist::TINYINTNULL;
    }
}

// Check whether val is NULL (or alternative NULL bit pattern for 64-bit string types)
template<ENUM_KIND KIND, typename T>
inline bool isNullValue(int64_t val, T NULL_VALUE)
{
    //@bug 339 might be a token here
    //TODO: what's up with the alternative NULL here?
    uint64_t ALT_NULL_VALUE = 0xFFFFFFFFFFFFFFFELL;

    constexpr int COL_WIDTH = sizeof(T);
    return (static_cast<T>(val) == NULL_VALUE) ||
           ((KIND_TEXT == KIND)  &&  (COL_WIDTH == 8)  &&  (val == ALT_NULL_VALUE));
}


/*****************************************************************************
 *** COMPARISON OPERATIONS FOR COLUMN VALUES *********************************
 *****************************************************************************/

bool isLike(const char* val, const idb_regex_t* regex)
{
    if (!regex)
        throw runtime_error("PrimitiveProcessor::isLike: Missing regular expression for LIKE operator");

#ifdef POSIX_REGEX
    return (regexec(&regex->regex, val, 0, NULL, 0) == 0);
#else
    return regex_match(val, regex->regex);
#endif
}

template<class T>
inline bool colCompare_(const T& val1, const T& val2, uint8_t COP)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2;

        case COMPARE_EQ:
            return val1 == val2;

        case COMPARE_LE:
            return val1 <= val2;

        case COMPARE_GT:
            return val1 > val2;

        case COMPARE_NE:
            return val1 != val2;

        case COMPARE_GE:
            return val1 >= val2;

        default:
            logIt(34, COP, "colCompare_");
            return false;						// throw an exception here?
    }
}

template<class T>
inline bool colCompare_(const T& val1, const T& val2, uint8_t COP, uint8_t rf)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2 || (val1 == val2 && (rf & 0x01));

        case COMPARE_LE:
            return val1 < val2 || (val1 == val2 && rf ^ 0x80);

        case COMPARE_EQ:
            return val1 == val2 && rf == 0;

        case COMPARE_NE:
            return val1 != val2 || rf != 0;

        case COMPARE_GE:
            return val1 > val2 || (val1 == val2 && rf ^ 0x01);

        case COMPARE_GT:
            return val1 > val2 || (val1 == val2 && (rf & 0x80));

        default:
            logIt(34, COP, "colCompare_l");
            return false;						// throw an exception here?
    }
}

//@bug 1828  Like must be a string compare.
inline bool colStrCompare_(uint64_t val1, uint64_t val2, uint8_t COP, uint8_t rf, const idb_regex_t* regex)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2 || (val1 == val2 && rf != 0);

        case COMPARE_LE:
            return val1 <= val2;

        case COMPARE_EQ:
            return val1 == val2 && rf == 0;

        case COMPARE_NE:
            return val1 != val2 || rf != 0;

        case COMPARE_GE:
            return val1 > val2 || (val1 == val2 && rf == 0);

        case COMPARE_GT:
            return val1 > val2;

        case COMPARE_LIKE:
        case COMPARE_NLIKE:
        {
            /* LIKE comparisons are string comparisons so we reverse the order again.
            	Switching the order twice is probably as efficient as evaluating a guard.  */
            char tmp[9];
            val1 = order_swap(val1);
            memcpy(tmp, &val1, 8);
            tmp[8] = '\0';
            return (COP & COMPARE_NOT ? !isLike(tmp, regex) : isLike(tmp, regex));
        }

        default:
            logIt(34, COP, "colStrCompare_");
            return false;						//TODO:  throw an exception here?
    }
}


// Compare two column values using given comparison operation,
// taking into account all rules about NULL values, string trimming and so on
template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL = false>
inline bool colCompare(
    int64_t val1,
    int64_t val2,
    uint8_t COP,
    uint8_t rf,
    const idb_regex_t& regex,
    bool isVal2Null = false)
{
// 	cout << "comparing " << hex << val1 << " to " << val2 << endl;

    if (COMPARE_NIL == COP) return false;

    //@bug 425 added IS_NULL condition
    else if (KIND_FLOAT == KIND  &&  !IS_NULL)
    {
        if (COL_WIDTH == 4)
        {
            float dVal1 = *((float*) &val1);
            float dVal2 = *((float*) &val2);
            return colCompare_(dVal1, dVal2, COP);
        }
        else
        {
            double dVal1 = *((double*) &val1);
            double dVal2 = *((double*) &val2);
            return colCompare_(dVal1, dVal2, COP);
        }
    }

    else if (KIND_TEXT == KIND  &&  !IS_NULL)
    {
        if (!regex.used && !rf)
        {
            // MCOL-1246 Trim trailing whitespace for matching, but not for regex
            dataconvert::DataConvert::trimWhitespace(val1);
            dataconvert::DataConvert::trimWhitespace(val2);
            return colCompare_(order_swap(val1), order_swap(val2), COP);
        }
        else
            return colStrCompare_(order_swap(val1), order_swap(val2), COP, rf, &regex);
    }

    else
    {
        if (IS_NULL == isVal2Null || (isVal2Null && COP == COMPARE_NE))
        {
            if (KIND_UNSIGNED == KIND)
            {
                uint64_t uval1 = val1, uval2 = val2;
                return colCompare_(uval1, uval2, COP, rf);
            }
            else
                return colCompare_(val1, val2, COP, rf);
        }
        else
            return false;
    }
}


/*****************************************************************************
 *** FILTER ENTIRE COLUMN ****************************************************
 *****************************************************************************/

// Provides 6 comparison operators for any datatype
template <int COP, typename T>
struct Comparator;

template <typename T>  struct Comparator<COMPARE_EQ, T>  {static bool compare(T val1, T val2)  {return val1 == val2;}};
template <typename T>  struct Comparator<COMPARE_NE, T>  {static bool compare(T val1, T val2)  {return val1 != val2;}};
template <typename T>  struct Comparator<COMPARE_GT, T>  {static bool compare(T val1, T val2)  {return val1 > val2;}};
template <typename T>  struct Comparator<COMPARE_LT, T>  {static bool compare(T val1, T val2)  {return val1 < val2;}};
template <typename T>  struct Comparator<COMPARE_GE, T>  {static bool compare(T val1, T val2)  {return val1 >= val2;}};
template <typename T>  struct Comparator<COMPARE_LE, T>  {static bool compare(T val1, T val2)  {return val1 <= val2;}};
template <typename T>  struct Comparator<COMPARE_NIL, T> {static bool compare(T val1, T val2)  {return false;}};

// Provides 3 combining operators for any flag type
template <int BOP, typename T>
struct Combiner;

template <typename T>  struct Combiner<BOP_AND, T>  {static void combine(T &flag, bool cmp)  {flag &= cmp;}};
template <typename T>  struct Combiner<BOP_OR,  T>  {static void combine(T &flag, bool cmp)  {flag |= cmp;}};
template <typename T>  struct Combiner<BOP_XOR, T>  {static void combine(T &flag, bool cmp)  {flag ^= cmp;}};


// Apply to dataArray[dataSize] column values the single filter element,
// consisting of comparison operator COP with a value to compare cmp_value,
// and combine the comparison result into the corresponding element of filterArray
// with combining operator BOP
template<int BOP, int COP, typename DATA_T, typename FILTER_ARRAY_T>
void applyFilterElement(
    size_t dataSize,
    const DATA_T* dataArray,
    DATA_T cmp_value,
    FILTER_ARRAY_T *filterArray)
{
    for (size_t i = 0; i < dataSize; ++i)
    {
        bool cmp = Comparator<COP, DATA_T>::compare(dataArray[i], cmp_value);
        Combiner<BOP,FILTER_ARRAY_T>::combine(filterArray[i], cmp);
    }
}

// Dispatch function by COP
template<int BOP, typename DATA_T, typename FILTER_ARRAY_T>
void applyFilterElement(
    int COP,
    size_t dataSize,
    const DATA_T* dataArray,
    DATA_T cmp_value,
    FILTER_ARRAY_T *filterArray)
{
    switch(COP)
    {
        case COMPARE_EQ:  applyFilterElement<BOP, COMPARE_EQ>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_NE:  applyFilterElement<BOP, COMPARE_NE>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_GT:  applyFilterElement<BOP, COMPARE_GT>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_LT:  applyFilterElement<BOP, COMPARE_LT>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_GE:  applyFilterElement<BOP, COMPARE_GE>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_LE:  applyFilterElement<BOP, COMPARE_LE>(dataSize, dataArray, cmp_value, filterArray);  break;
        case COMPARE_NIL: applyFilterElement<BOP, COMPARE_NIL>(dataSize,dataArray, cmp_value, filterArray);  break;
        default:          idbassert(0);
    }
}

template<int BOP, typename DATA_T, typename FILTER_ARRAY_T>
void applySetFilter(
    size_t dataSize,
    const DATA_T* dataArray,
    prestored_set_t* filterSet,     // Set of values for simple filters (any of values / none of them)
    FILTER_ARRAY_T *filterArray)
{
    for (size_t i = 0; i < dataSize; ++i)
    {
        bool found = (filterSet->find(dataArray[i]) != filterSet->end());
        filterArray[i] = (BOP_OR == BOP?  found : !found);
    }
}


/*****************************************************************************
 *** FILTER A COLUMN VALUE ***************************************************
 *****************************************************************************/

// Return true if curValue matches the filter represented by all those arrays
template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL = false, typename T>
inline bool matchingColValue(
    // Value description
    int64_t curValue,               // The value (IS_NULL - is the value null?)
    // Filter description
    ColumnFilterMode columnFilterMode,
    prestored_set_t* filterSet,     // Set of values for simple filters (any of values / none of them)
    uint32_t filterCount,           // Number of filter elements, each described by one entry in the following arrays:
    uint8_t* filterCOPs,            //   comparison operation
    int64_t* filterValues,          //   value to compare to
    uint8_t* filterRFs,
    idb_regex_t* filterRegexes,     //   regex for string-LIKE comparison operation
    T NULL_VALUE)                   // Bit pattern representing NULL value for this column type/width
{
    /* In order to make filtering as fast as possible, we replaced the single generic algorithm
       with several algorithms, better tailored for more specific cases:
       empty filter, single comparison, and/or/xor comparison results, one/none of small/large set of values
    */
    switch (columnFilterMode)
    {
        // Empty filter is always true
        case ALWAYS_TRUE:
            return true;


        // Filter consisting of exactly one comparison operation
        case SINGLE_COMPARISON:
        {
            auto filterValue = filterValues[0];
            bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[0],
                                                            filterRFs[0], filterRegexes[0],
                                                            isNullValue<KIND,T>(filterValue, NULL_VALUE));
            return cmp;
        }


        // Filter is true if ANY comparison is true (BOP_OR)
        case ANY_COMPARISON_TRUE:
        {
            for (int argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], filterRegexes[argIndex],
                                                                isNullValue<KIND,T>(filterValue, NULL_VALUE));

                // Short-circuit the filter evaluation - true || ... == true
                if (cmp == true)
                    return true;
            }

            // We can get here only if all filters returned false
            return false;
        }


        // Filter is true only if ALL comparisons are true (BOP_AND)
        case ALL_COMPARISONS_TRUE:
        {
            for (int argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], filterRegexes[argIndex],
                                                                isNullValue<KIND,T>(filterValue, NULL_VALUE));

                // Short-circuit the filter evaluation - false && ... = false
                if (cmp == false)
                    return false;
            }

            // We can get here only if all filters returned true
            return true;
        }


        // XORing results of comparisons (BOP_XOR)
        case XOR_COMPARISONS:
        {
            bool result = false;

            for (int argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], filterRegexes[argIndex],
                                                                isNullValue<KIND,T>(filterValue, NULL_VALUE));
                result ^= cmp;
            }

            return result;
        }


        // ONE of the values in the small set represented by an array (BOP_OR + all COMPARE_EQ)
        case ONE_OF_VALUES_IN_ARRAY:
        {
            for (int argIndex = 0; argIndex < filterCount; argIndex++)
            {
                if (curValue == filterValues[argIndex])
                    return true;
            }

            return false;
        }


        // NONE of the values in the small set represented by an array (BOP_AND + all COMPARE_NE)
        case NONE_OF_VALUES_IN_ARRAY:
        {
            for (int argIndex = 0; argIndex < filterCount; argIndex++)
            {
                if (curValue == filterValues[argIndex])
                    return false;
            }

            return true;
        }


        // ONE of the values in the set is equal to the value checked (BOP_OR + all COMPARE_EQ)
        case ONE_OF_VALUES_IN_SET:
        {
            bool found = (filterSet->find(curValue) != filterSet->end());
            return found;
        }


        // NONE of the values in the set is equal to the value checked (BOP_AND + all COMPARE_NE)
        case NONE_OF_VALUES_IN_SET:
        {
            // bug 1920: ignore NULLs in the set and in the column data
            if (IS_NULL)
                return false;

            bool found = (filterSet->find(curValue) != filterSet->end());
            return !found;
        }


        default:
            idbassert(0);
            return true;
    }
}


// Set the minimum and maximum in the return header if we will be doing a block scan and
// we are dealing with a type that is comparable as a 64 bit integer.  Subsequent calls can then
// skip this block if the value being searched is outside of the Min/Max range.
bool isMinMaxValid(const NewColRequestHeader* in)
{
    if (in->NVALS != 0)
    {
        return false;
    }
    else
    {
        switch (in->DataType)
        {
            case CalpontSystemCatalog::CHAR:
                return (in->DataSize < 9);

            case CalpontSystemCatalog::VARCHAR:
            case CalpontSystemCatalog::BLOB:
            case CalpontSystemCatalog::TEXT:
                return (in->DataSize < 8);

            case CalpontSystemCatalog::TINYINT:
            case CalpontSystemCatalog::SMALLINT:
            case CalpontSystemCatalog::MEDINT:
            case CalpontSystemCatalog::INT:
            case CalpontSystemCatalog::DATE:
            case CalpontSystemCatalog::BIGINT:
            case CalpontSystemCatalog::DATETIME:
            case CalpontSystemCatalog::TIME:
            case CalpontSystemCatalog::TIMESTAMP:
            case CalpontSystemCatalog::UTINYINT:
            case CalpontSystemCatalog::USMALLINT:
            case CalpontSystemCatalog::UMEDINT:
            case CalpontSystemCatalog::UINT:
            case CalpontSystemCatalog::UBIGINT:
                return true;

            case CalpontSystemCatalog::DECIMAL:
            case CalpontSystemCatalog::UDECIMAL:
                return (in->DataSize <= 8);

            default:
                return false;
        }
    }
}


/*****************************************************************************
 *** READ COLUMN VALUES ******************************************************
 *****************************************************************************/

// Read one ColValue from the input data.
// Return true on success, false on EOF.
// Values are read from srcArray either in natural order or in the order defined by ridArray.
// Empty values are skipped, unless ridArray==0 && !(OutputType & OT_RID).
template<typename T, int COL_WIDTH>
inline bool nextColValue(
    int64_t* result,            // Place for the value returned
    bool* isEmpty,              // ... and flag whether it's EMPTY
    int* index,                 // Successive index either in srcArray (going from 0 to srcSize-1) or ridArray (0..ridSize-1)
    uint16_t* rid,              // Index in srcArray of the value returned
    const T* srcArray,          // Input array
    const unsigned srcSize,     // ... and its size
    const uint16_t* ridArray,   // Optional array of indexes into srcArray, that defines the read order
    const int ridSize,          // ... and its size
    const uint8_t OutputType,   // Used to decide whether to skip EMPTY values
    T EMPTY_VALUE)
{
    auto i = *index;    // local copy of *index to speed up loops
    T value;            // value to be written into *result, local for the same reason

    if (ridArray)
    {
        // Read next non-empty value in the order defined by ridArray
        for( ; ; i++)
        {
            if (UNLIKELY(i >= ridSize))
                return false;

            value = srcArray[ridArray[i]];

            if (value != EMPTY_VALUE)
                break;
        }

        *rid = ridArray[i];
        *isEmpty = false;
    }
    else if (OutputType & OT_RID)   //TODO: check correctness of this condition for SKIP_EMPTY_VALUES
    {
        // Read next non-empty value in the natural order
        for( ; ; i++)
        {
            if (UNLIKELY(i >= srcSize))
                return false;

            value = srcArray[i];

            if (value != EMPTY_VALUE)
                break;
        }

        *rid = i;
        *isEmpty = false;
    }
    else
    {
        // Read next value in the natural order
        if (UNLIKELY(i >= srcSize))
            return false;

        *rid = i;
        value = srcArray[i];
        *isEmpty = (value == EMPTY_VALUE);
    }

    //Bug 838, tinyint null problem
#if 0
    if (type == CalpontSystemCatalog::FLOAT)
    {
        // convert the float to a 64-bit type, return that w/o conversion
        double dTmp = (double) * ((float*) &srcArray[*rid]);
        *result = *((int64_t*) &dTmp);
    }
    else
        *result = srcArray[*rid];
#endif

    *index = i+1;
    *result = value;
    return true;
}


/* Scan srcArray[srcSize] either in the natural order
   or in the order provided by ridArray[ridSize] (when RID_ORDER==true),
   When SKIP_EMPTY_VALUES==true, skip values equal to EMPTY_VALUE.
   Save non-skipped values to dataArray[] and, when WRITE_RID==true, their indexes to dataRid[].
   Return number of values written to dataArray[]
*/
template <bool WRITE_RID, bool RID_ORDER, bool SKIP_EMPTY_VALUES, typename SRC_T, typename DST_T>
size_t readArray(
    const SRC_T* srcArray, size_t srcSize,
    DST_T* dataArray, RID_T* dataRid = NULL,
    const RID_T* ridArray = NULL, size_t ridSize = 0,
    const SRC_T EMPTY_VALUE = 0)
{
    // Depending on RID_ORDER, we will scan either ridSize elements of ridArray[] or srcSize elements of srcArray[]
    size_t inputSize = (RID_ORDER? ridSize : srcSize);
    auto out = dataArray;

    // Check that all employed arrays are non-NULL.
    // NOTE: unused arays may still be non-NULL in order to simplify calling code.
    idbassert(srcArray);
    idbassert(dataArray);
    if (RID_ORDER)
        idbassert(ridArray);
    if (WRITE_RID)
        idbassert(dataRid);
    if (SKIP_EMPTY_VALUES)
        idbassert(EMPTY_VALUE);

    for(size_t i=0; i < inputSize; i++)
    {
        size_t rid = (RID_ORDER? ridArray[i] : i);
        auto value = srcArray[rid];

        if (SKIP_EMPTY_VALUES? LIKELY(value != EMPTY_VALUE) : true)
        {
            *out++ = static_cast<DST_T>(value);

            if (WRITE_RID)
                *dataRid++ = rid;
        }
    }

    return out - dataArray;
}


/*****************************************************************************
 *** WRITE COLUMN VALUES *****************************************************
 *****************************************************************************/

// Append value to the output buffer with debug-time check for buffer overflow
template<typename T>
inline void checkedWriteValue(
    void* out,
    unsigned outSize,
    unsigned* outPos,
    const T* src,
    int errSubtype)
{
#ifdef PRIM_DEBUG

    if (sizeof(T) > outSize - *outPos)
    {
        logIt(35, errSubtype);
        throw logic_error("PrimitiveProcessor::checkedWriteValue(): output buffer is too small");
    }

#endif

    uint8_t* out8 = reinterpret_cast<uint8_t*>(out);
    memcpy(out8 + *outPos, src, sizeof(T));
    *outPos += sizeof(T);
}


// Write the value index in srcArray and/or the value itself, depending on bits in OutputType,
// into the output buffer and update the output pointer.
template<typename T>
inline void writeColValue(
    uint8_t OutputType,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written,
    uint16_t rid,
    const T* srcArray)
{
    if (OutputType & OT_RID)
    {
        checkedWriteValue(out, outSize, written, &rid, 1);
        out->RidFlags |= (1 << (rid >> 10)); // set the (row/1024)'th bit
    }

    if (OutputType & (OT_TOKEN | OT_DATAVALUE))
    {
        checkedWriteValue(out, outSize, written, &srcArray[rid], 2);
    }

    out->NVALS++;   //TODO: Can be computed at the end from *written value
}


template <bool WRITE_RID, bool WRITE_DATA, typename FILTER_ARRAY_T, typename RID_T, typename T>
void writeArray(
    size_t dataSize,
    const T* dataArray,
    const RID_T* dataRid,
    const FILTER_ARRAY_T *filterArray,
    uint8_t* outbuf,
    unsigned* written,
    uint16_t* NVALS,
    uint8_t* RidFlagsPtr,
    bool isNullValueMatches,
    T NULL_VALUE)
{
    uint8_t* out = outbuf;
    uint8_t RidFlags = *RidFlagsPtr;

    for (size_t i = 0; i < dataSize; ++i)
    {
        if (dataArray[i]==NULL_VALUE? isNullValueMatches : filterArray[i])
        {
            if (WRITE_RID)
            {
                copyValue(out, &dataRid[i], sizeof(RID_T));
                out += sizeof(RID_T);

                RidFlags |= (1 << (dataRid[i] >> 10)); // set the (row/1024)'th bit
            }

            if (WRITE_DATA)
            {
                copyValue(out, &dataArray[i], sizeof(T));
                out += sizeof(T);
            }
        }
    }

    // Update number of written values and number of written bytes
    int size1 = (WRITE_RID? sizeof(RID_T) : 0) + (WRITE_DATA? sizeof(T) : 0);
    *NVALS += (out - outbuf) / size1;
    *written += out - outbuf;
    *RidFlagsPtr = RidFlags;
}


/*****************************************************************************
 *** COMPILE A COLUMN FILTER *************************************************
 *****************************************************************************/

// Compile column filter from BLOB into structure optimized for fast filtering.
// Return the compiled filter.
template<typename T>                // C++ integer type providing storage for colType
boost::shared_ptr<ParsedColumnFilter> parseColumnFilter_T(
    const uint8_t* filterString,    // Filter represented as BLOB
    uint32_t colType,               // Column datatype as ColDataType
    uint32_t filterCount,           // Number of filter elements contained in filterString
    uint32_t BOP)                   // Operation (and/or/xor/none) that combines all filter elements
{
    const uint32_t COL_WIDTH = sizeof(T);  // Sizeof of the column to be filtered

    boost::shared_ptr<ParsedColumnFilter> ret;  // Place for building the value to return
    if (filterCount == 0)
        return ret;

    // Allocate the compiled filter structure with space for filterCount filters.
    // No need to init arrays since they will be filled on the fly.
    ret.reset(new ParsedColumnFilter());
    ret->prestored_argVals.reset(new int64_t[filterCount]);
    ret->prestored_cops.reset(new uint8_t[filterCount]);
    ret->prestored_rfs.reset(new uint8_t[filterCount]);
    ret->prestored_regex.reset(new idb_regex_t[filterCount]);

    // Choose initial filter mode based on operation and number of filter elements
    if (filterCount == 1)
        ret->columnFilterMode = SINGLE_COMPARISON;
    else if (BOP == BOP_OR)
        ret->columnFilterMode = ANY_COMPARISON_TRUE;
    else if (BOP == BOP_AND)
        ret->columnFilterMode = ALL_COMPARISONS_TRUE;
    else if (BOP == BOP_XOR)
        ret->columnFilterMode = XOR_COMPARISONS;
    else
        idbassert(0);   // BOP_NONE is compatible only with filterCount <= 1


    // Parse the filter predicates and insert them into argVals and cops
    for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
    {
        // Size of single filter element in filterString BLOB
        const uint32_t filterSize = sizeof(uint8_t) + sizeof(uint8_t) + COL_WIDTH;

        // Pointer to ColArgs structure representing argIndex'th element in the BLOB
        auto args = reinterpret_cast<const ColArgs*>(filterString + (argIndex * filterSize));

        ret->prestored_cops[argIndex] = args->COP;
        ret->prestored_rfs[argIndex] = args->rf;

#if 0
        if (colType == CalpontSystemCatalog::FLOAT)
        {
            double dTmp;

            dTmp = (double) * ((const float*) args->val);
            ret->prestored_argVals[argIndex] = *((int64_t*) &dTmp);
        }
        else
#else
        ret->prestored_argVals[argIndex] = *reinterpret_cast<const T*>(args->val);
#endif

//      cout << "inserted* " << hex << ret->prestored_argVals[argIndex] << dec <<
//        " COP = " << (int) ret->prestored_cops[argIndex] << endl;

        bool useRegex = ((COMPARE_LIKE & args->COP) != 0);
        ret->prestored_regex[argIndex].used = useRegex;

        if (useRegex)
        {
            p_DataValue dv = convertToPDataValue(&ret->prestored_argVals[argIndex], COL_WIDTH);
            if (PrimitiveProcessor::convertToRegexp(&ret->prestored_regex[argIndex], &dv))
                throw runtime_error("PrimitiveProcessor::parseColumnFilter_T(): Could not create regular expression for LIKE operator");
        }
    }


    /*  Decide which structure to use.  I think the only cases where we can use the set
        are when NOPS > 1, BOP is OR, and every COP is ==,
        and when NOPS > 1, BOP is AND, and every COP is !=.

        If there were no predicates that violate the condition for using a set,
        insert argVals into a set.
    */
    if (filterCount > 1)
    {
        // Check that all COPs are of right kind that depends on BOP
        for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
        {
            auto cop = ret->prestored_cops[argIndex];

            if (! ((BOP == BOP_OR  && cop == COMPARE_EQ) ||
                   (BOP == BOP_AND && cop == COMPARE_NE)))
            {
                goto skipConversion;
            }
        }


        // Now we found that conversion is possible. Let's choose between array-based search
        // and set-based search depending on the set size.
        //TODO: Tailor the threshold based on the actual search algorithms used and COL_WIDTH/SIMD_WIDTH

        if (filterCount <= 8)
        {
            // Assign filter mode of array-based filtering
            if (BOP == BOP_OR)
                ret->columnFilterMode = ONE_OF_VALUES_IN_ARRAY;
            else
                ret->columnFilterMode = NONE_OF_VALUES_IN_ARRAY;
        }
        else
        {
            // Assign filter mode of set-based filtering
            if (BOP == BOP_OR)
                ret->columnFilterMode = ONE_OF_VALUES_IN_SET;
            else
                ret->columnFilterMode = NONE_OF_VALUES_IN_SET;

            // @bug 2584, use COMPARE_NIL for "= null" to allow "is null" in OR expression
            ret->prestored_set.reset(new prestored_set_t());
            for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
                if (ret->prestored_rfs[argIndex] == 0)
                    ret->prestored_set->insert(ret->prestored_argVals[argIndex]);
        }

        skipConversion:;
    }

    return ret;
}


/*****************************************************************************
 *** RUN DATA THROUGH A COLUMN FILTER ****************************************
 *****************************************************************************/

/* "Vertical" processing of the column filter:
   1. load all data into temporary vector
   2. process one filter element over entire vector before going to a next one
   3. write records, that succesfully passed through the filter, to outbuf
*/
template<typename T, ENUM_KIND KIND>
void processArray(
    // Source data
    const T* srcArray,
    size_t srcSize,
    uint16_t* ridArray,
    size_t ridSize,                 // Number of values in ridArray
    // Filter description
    int BOP,
    prestored_set_t* filterSet,     // Set of values for simple filters (any of values / none of them)
    uint32_t filterCount,           // Number of filter elements, each described by one entry in the following arrays:
    uint8_t* filterCOPs,            //   comparison operation
    int64_t* filterValues,          //   value to compare to
    // Output buffer/stats
    uint8_t* outbuf,                // Pointer to the place for output data
    unsigned* written,              // Number of written bytes, that we need to update
    uint16_t* NVALS,                // Number of written values, that we need to update
    uint8_t* RidFlagsPtr,           // Pointer to out->RidFlags
    // Processing parameters
    bool WRITE_RID,
    bool WRITE_DATA,
    bool SKIP_EMPTY_VALUES,
    T EMPTY_VALUE,
    bool isNullValueMatches,
    T NULL_VALUE)
{
    // Alloc temporary arrays
    size_t inputSize = (ridArray? ridSize : srcSize);

    // Temporary array with data to filter
    std::vector<T> dataVec(inputSize);
    auto dataArray = dataVec.data();

    // Temporary array with RIDs of corresponding dataArray elements
    std::vector<RID_T> dataRidVec(WRITE_RID? inputSize : 0);
    auto dataRid = dataRidVec.data();


    // Copy input data into temporary array, opt. storing RIDs, opt. skipping EMPTYs
    size_t dataSize;  // number of values copied into dataArray
    if (ridArray != NULL)
    {
        dataSize = WRITE_RID? readArray<true, true,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,true,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }
    else if (SKIP_EMPTY_VALUES)
    {
        dataSize = WRITE_RID? readArray<true, false,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,false,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }
    else
    {
        dataSize = WRITE_RID? readArray<true, false,false>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,false,false>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }


    // Choose initial filterArray[i] value depending on the operation
    bool initValue = false;
    if      (filterCount == 0) {initValue = true;}
    else if (BOP_NONE == BOP)  {initValue = false;  BOP = BOP_OR;}
    else if (BOP_OR   == BOP)  {initValue = false;}
    else if (BOP_XOR  == BOP)  {initValue = false;}
    else if (BOP_AND  == BOP)  {initValue = true;}

    // Temporary array accumulating results of filtering for each record
    std::vector<uint8_t> filterVec(dataSize, initValue);
    auto filterArray = filterVec.data();

    // Real type of column data, may be floating-point (used only in the filtering)
    using FLOAT_T = typename std::conditional<sizeof(T) == 8, double, float>::type;
    using DATA_T  = typename std::conditional<KIND_FLOAT == KIND, FLOAT_T, T>::type;
    auto realDataArray = reinterpret_cast<DATA_T*>(dataArray);


    //prepareArray();

    if (filterSet != NULL  &&  BOP == BOP_OR)
    {
        applySetFilter<BOP_OR>(dataSize, realDataArray, filterSet, filterArray);
    }
    else if (filterSet != NULL  &&  BOP == BOP_AND)
    {
        applySetFilter<BOP_AND>(dataSize, realDataArray, filterSet, filterArray);
    }
    else
    {
        for (int i = 0; i < filterCount; ++i)
        {
            DATA_T cmp_value;   // value for comparison, may be floating-point
            copyValue(&cmp_value, &filterValues[i], sizeof(cmp_value));

            switch(BOP)
            {
                case BOP_AND:  applyFilterElement<BOP_AND>(filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                case BOP_OR:   applyFilterElement<BOP_OR> (filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                case BOP_XOR:  applyFilterElement<BOP_XOR>(filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                default:       idbassert(0);
            }
        }
    }


    // Copy filtered data and/or their RIDs into output buffer
    if (WRITE_RID && WRITE_DATA)
        writeArray<true,true> (dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, isNullValueMatches, NULL_VALUE);
    else if (WRITE_RID)
        writeArray<true,false>(dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, isNullValueMatches, NULL_VALUE);
    else
        writeArray<false,true>(dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, isNullValueMatches, NULL_VALUE);
}


// Copy data matching parsedColumnFilter from input to output.
// Input is srcArray[srcSize], optionally accessed in the order defined by ridArray[ridSize].
// Output is BLOB out[outSize], written starting at offset *written, which is updated afterward.
template<typename T, ENUM_KIND KIND>
void filterColumnData(
    NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written,
    uint16_t* ridArray,
    int ridSize,                // Number of values in ridArray
    int* srcArray16,
    unsigned srcSize,
    boost::shared_ptr<ParsedColumnFilter> parsedColumnFilter)
{
    constexpr int COL_WIDTH = sizeof(T);
    const T* srcArray = reinterpret_cast<const T*>(srcArray16);

    // Cache some structure fields in local vars
    auto DataType = (CalpontSystemCatalog::ColDataType) in->DataType;  // Column datatype
    uint32_t filterCount = in->NOPS;        // Number of elements in the filter
    uint8_t  OutputType  = in->OutputType;

    // If no pre-parsed column filter is set, parse the filter in the message
    if (parsedColumnFilter.get() == NULL  &&  filterCount > 0)
        parsedColumnFilter = parseColumnFilter_T<T>((uint8_t*)in + sizeof(NewColRequestHeader), in->DataType, in->NOPS, in->BOP);

    // Cache parsedColumnFilter fields in local vars
    auto columnFilterMode = (filterCount==0? ALWAYS_TRUE : parsedColumnFilter->columnFilterMode);
    auto filterValues  = (filterCount==0? NULL : parsedColumnFilter->prestored_argVals.get());
    auto filterCOPs    = (filterCount==0? NULL : parsedColumnFilter->prestored_cops.get());
    auto filterRFs     = (filterCount==0? NULL : parsedColumnFilter->prestored_rfs.get());
    auto filterSet     = (filterCount==0? NULL : parsedColumnFilter->prestored_set.get());
    auto filterRegexes = (filterCount==0? NULL : parsedColumnFilter->prestored_regex.get());

    // Bit patterns in srcArray[i] representing EMPTY and NULL values
    T EMPTY_VALUE = static_cast<T>(getEmptyValue<COL_WIDTH>(DataType));
    T NULL_VALUE  = static_cast<T>(getNullValue <COL_WIDTH>(DataType));

    // Precompute filter results for EMPTY and NULL values
    bool isEmptyValueMatches = matchingColValue<KIND, COL_WIDTH, false>(EMPTY_VALUE, columnFilterMode, filterSet, filterCount,
                                    filterCOPs, filterValues, filterRFs, filterRegexes, NULL_VALUE);

    bool isNullValueMatches = matchingColValue<KIND, COL_WIDTH, true>(NULL_VALUE, columnFilterMode, filterSet, filterCount,
                                    filterCOPs, filterValues, filterRFs, filterRegexes, NULL_VALUE);

    // Boolean indicating whether to capture the min and max values
    bool ValidMinMax = isMinMaxValid(in);
    // Real type of values captured in Min/Max
    using VALTYPE = typename std::conditional<KIND_UNSIGNED == KIND, uint64_t, int64_t>::type;
    // Local vars to capture the min and max values
    auto Min = static_cast<int64_t>(numeric_limits<VALTYPE>::max());
    auto Max = static_cast<int64_t>(numeric_limits<VALTYPE>::min());


    // If possible, use faster "vertical" filtering approach
    if (0  &&  KIND != KIND_TEXT)
    {
        ////TODO: handling MinMax

        bool canUseFastFiltering = true;
        for (int i = 0; i < filterCount; ++i)
            if (filterRFs[i] != 0)
                canUseFastFiltering = false;

        if (canUseFastFiltering)
        {
            processArray<T, KIND>(srcArray, srcSize, ridArray, ridSize,
                         in->BOP, filterSet, filterCount, filterCOPs, filterValues,
                         reinterpret_cast<uint8_t*>(out) + *written,
                         written, & out->NVALS, & out->RidFlags,
                         (OutputType & OT_RID) != 0,
                         (OutputType & (OT_TOKEN | OT_DATAVALUE)) != 0,
                         (OutputType & OT_RID) != 0,  //TODO: check correctness of this condition for SKIP_EMPTY_VALUES
                         EMPTY_VALUE,
                         isNullValueMatches,
                         NULL_VALUE);
            return;
        }
    }


    // Loop-local variables
    int64_t curValue = 0;
    uint16_t rid = 0;
    bool isEmpty = false;
    idb_regex_t placeholderRegex;
    placeholderRegex.used = false;

    // Loop over the column values, storing those matching the filter, and updating the min..max range
    for (int i = 0;
         nextColValue<T, COL_WIDTH>(&curValue, &isEmpty,
                                    &i, &rid,
                                    srcArray, srcSize, ridArray, ridSize,
                                    OutputType, EMPTY_VALUE); )
    {
        if (isEmpty)
        {
            // If EMPTY values match the filter, write curValue to the output buffer
            if (isEmptyValueMatches)
                writeColValue<T>(OutputType, out, outSize, written, rid, srcArray);
        }
        else if (isNullValue<KIND,T>(curValue, NULL_VALUE))
        {
            // If NULL values match the filter, write curValue to the output buffer
            if (isNullValueMatches)
                writeColValue<T>(OutputType, out, outSize, written, rid, srcArray);
        }
        else
        {
            // If curValue matches the filter, write it to the output buffer
            if (matchingColValue<KIND, COL_WIDTH, false>(curValue, columnFilterMode, filterSet, filterCount,
                                filterCOPs, filterValues, filterRFs, filterRegexes, NULL_VALUE))
            {
                writeColValue<T>(OutputType, out, outSize, written, rid, srcArray);
            }

            // Update Min and Max if necessary.  EMPTY/NULL values are processed in other branches.
            if (ValidMinMax)
            {
                if ((KIND_TEXT == KIND) && (COL_WIDTH > 1))
                {
                    // When computing Min/Max for string fields, we compare them trimWhitespace()'d
                    if (colCompare<KIND, COL_WIDTH>(Min, curValue, COMPARE_GT, false, placeholderRegex))
                        Min = curValue;

                    if (colCompare<KIND, COL_WIDTH>(Max, curValue, COMPARE_LT, false, placeholderRegex))
                        Max = curValue;
                }
                else
                {
                    if (static_cast<VALTYPE>(Min) > static_cast<VALTYPE>(curValue))
                        Min = curValue;

                    if (static_cast<VALTYPE>(Max) < static_cast<VALTYPE>(curValue))
                        Max = curValue;
                }
            }
        }
    }


    // Write captured Min/Max values to *out
    out->ValidMinMax = ValidMinMax;
    if (ValidMinMax)
    {
        out->Min = Min;
        out->Max = Max;
    }
}

} //namespace anon

namespace primitives
{

void PrimitiveProcessor::p_Col(NewColRequestHeader* in, NewColResultHeader* out,
                               unsigned outSize, unsigned* written)
{
    void *outp = static_cast<void*>(out);
    memcpy(outp, in, sizeof(ISMPacketHeader) + sizeof(PrimitiveHeader));
    out->NVALS = 0;
    out->LBID = in->LBID;
    out->ism.Command = COL_RESULTS;
    out->OutputType = in->OutputType;
    out->RidFlags = 0;
    *written = sizeof(NewColResultHeader);
    unsigned itemsPerBlock = logicalBlockMode ? BLOCK_SIZE
                                              : BLOCK_SIZE / in->DataSize;

    //...Initialize I/O counts;
    out->CacheIO    = 0;
    out->PhysicalIO = 0;

#if 0

    // short-circuit the actual block scan for testing
    if (out->LBID >= 802816)
    {
        out->ValidMinMax = false;
        out->Min = 0;
        out->Max = 0;
        return;
    }

#endif

    auto markEvent = [&] (char eventChar)
    {
        if (fStatsPtr)
#ifdef _MSC_VER
            fStatsPtr->markEvent(in->LBID, GetCurrentThreadId(), in->hdr.SessionID, eventChar);
#else
            fStatsPtr->markEvent(in->LBID, pthread_self(), in->hdr.SessionID, eventChar);
#endif
    };

    markEvent('B');

    // Prepare ridArray (the row index array)
    uint16_t* ridArray = 0;
    int ridSize = in->NVALS;                // Number of values in ridArray
    if (ridSize > 0)
    {
        int filterSize = sizeof(uint8_t) + sizeof(uint8_t) + in->DataSize;
        ridArray = reinterpret_cast<uint16_t*>((uint8_t*)in + sizeof(NewColRequestHeader) + (in->NOPS * filterSize));

        if (1 == in->sort )
        {
            std::sort(ridArray, ridArray + ridSize);
            markEvent('O');
        }
    }

    auto DataType = (CalpontSystemCatalog::ColDataType) in->DataType;

    // Dispatch filtering by the column datatype/width in order to make it faster
    if (DataType == CalpontSystemCatalog::FLOAT)
    {
        idbassert(in->DataSize == 4);
        filterColumnData<int32_t, KIND_FLOAT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
    }
    else if (DataType == CalpontSystemCatalog::DOUBLE)
    {
        idbassert(in->DataSize == 8);
        filterColumnData<int64_t, KIND_FLOAT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
    }
    else if (DataType == CalpontSystemCatalog::CHAR ||
             DataType == CalpontSystemCatalog::VARCHAR ||
             DataType == CalpontSystemCatalog::TEXT)
    {
        switch (in->DataSize)
        {
            case 1:  filterColumnData< int8_t, KIND_TEXT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 2:  filterColumnData<int16_t, KIND_TEXT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 4:  filterColumnData<int32_t, KIND_TEXT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 8:  filterColumnData<int64_t, KIND_TEXT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            default: idbassert(0);
        }
    }
    else if (isUnsigned(DataType))
    {
        switch (in->DataSize)
        {
            case 1:  filterColumnData< uint8_t, KIND_UNSIGNED>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 2:  filterColumnData<uint16_t, KIND_UNSIGNED>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 4:  filterColumnData<uint32_t, KIND_UNSIGNED>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 8:  filterColumnData<uint64_t, KIND_UNSIGNED>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            default: idbassert(0);
        }
    }
    else
    {
        switch (in->DataSize)
        {
            case 1:  filterColumnData< int8_t, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 2:  filterColumnData<int16_t, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 4:  filterColumnData<int32_t, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            case 8:  filterColumnData<int64_t, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);  break;
            default: idbassert(0);
        }
    }

    markEvent('C');
}


// Compile column filter from BLOB into structure optimized for fast filtering.
// Returns the compiled filter.
boost::shared_ptr<ParsedColumnFilter> parseColumnFilter(
    const uint8_t* filterString,    // Filter represented as BLOB
    uint32_t colWidth,              // Sizeof of the column to be filtered
    uint32_t colType,               // Column datatype as ColDataType
    uint32_t filterCount,           // Number of filter elements contained in filterString
    uint32_t BOP)                   // Operation (and/or/xor/none) that combines all filter elements
{
    // Dispatch by the column type to make it faster
    if (isUnsigned((CalpontSystemCatalog::ColDataType)colType))
    {
        switch (colWidth)
        {
            case 1:  return parseColumnFilter_T< uint8_t>(filterString, colType, filterCount, BOP);
            case 2:  return parseColumnFilter_T<uint16_t>(filterString, colType, filterCount, BOP);
            case 4:  return parseColumnFilter_T<uint32_t>(filterString, colType, filterCount, BOP);
            case 8:  return parseColumnFilter_T<uint64_t>(filterString, colType, filterCount, BOP);
        }
    }
    else
    {
        switch (colWidth)
        {
            case 1:  return parseColumnFilter_T< int8_t>(filterString, colType, filterCount, BOP);
            case 2:  return parseColumnFilter_T<int16_t>(filterString, colType, filterCount, BOP);
            case 4:  return parseColumnFilter_T<int32_t>(filterString, colType, filterCount, BOP);
            case 8:  return parseColumnFilter_T<int64_t>(filterString, colType, filterCount, BOP);
        }
    }

    logIt(36, colType*100 + colWidth, "parseColumnFilter");
    return NULL;   //FIXME: support for wider columns
}

} // namespace primitives
// vim:ts=4 sw=4:

