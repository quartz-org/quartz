#ifndef DATAFRAME_H
#define DATAFRAME_H

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <variant>
#include <functional>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <optional>
#include <immintrin.h>
#include <cstdint>

namespace qz_data {

// ============================================================================
// Data Types for DataFrame Columns
// ============================================================================

enum class DType {
    Int32,
    Int64,
    Float32,
    Float64,
    String,
    Bool,
    DateTime,  // Stored as int64 (unix timestamp ms)
    Null
};

inline const char* dtypeToString(DType dt) {
    switch (dt) {
        case DType::Int32: return "int32";
        case DType::Int64: return "int64";
        case DType::Float32: return "float32";
        case DType::Float64: return "float64";
        case DType::String: return "string";
        case DType::Bool: return "bool";
        case DType::DateTime: return "datetime";
        case DType::Null: return "null";
    }
    return "unknown";
}

// ============================================================================
// Series: A single column of homogeneous data
// ============================================================================

class Series {
public:
    // Constructors
    Series() : dtype_(DType::Null), name_("") {}
    explicit Series(const std::string& name) : dtype_(DType::Null), name_(name) {}
    Series(const std::string& name, const std::vector<double>& data);
    Series(const std::string& name, const std::vector<int64_t>& data);
    Series(const std::string& name, const std::vector<std::string>& data);
    Series(const std::string& name, const std::vector<bool>& data);
    
    // Static factory methods
    static Series fromDoubles(const std::string& name, const std::vector<double>& data);
    static Series fromInts(const std::string& name, const std::vector<int64_t>& data);
    static Series fromStrings(const std::string& name, const std::vector<std::string>& data);
    static Series fromBools(const std::string& name, const std::vector<bool>& data);
    static Series range(const std::string& name, int64_t start, int64_t end, int64_t step = 1);
    static Series zeros(const std::string& name, size_t count);
    static Series ones(const std::string& name, size_t count);
    static Series constant(const std::string& name, double value, size_t count);
    
    // Basic info
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    DType dtype() const { return dtype_; }
    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    
    // Element access (returns NaN for invalid index)
    double getDouble(size_t idx) const;
    int64_t getInt(size_t idx) const;
    std::string getString(size_t idx) const;
    bool getBool(size_t idx) const;
    bool isNull(size_t idx) const;
    
    // Bulk data access (for performance)
    const double* doubleData() const { return float64_data_.data(); }
    const int64_t* intData() const { return int64_data_.data(); }
    const std::string* stringData() const { return string_data_.data(); }
    
    // Mutators
    void setDouble(size_t idx, double val);
    void setInt(size_t idx, int64_t val);
    void setString(size_t idx, const std::string& val);
    void setBool(size_t idx, bool val);
    void setNull(size_t idx);
    void pushDouble(double val);
    void pushInt(int64_t val);
    void pushString(const std::string& val);
    void pushBool(bool val);
    void reserve(size_t capacity);
    
    // NULL handling
    size_t nullCount() const;
    std::vector<size_t> nullIndices() const;
    Series dropNull() const;
    Series fillNull(double value) const;
    Series fillNullForward() const;
    Series fillNullBackward() const;
    
    // ========================================================================
    // Aggregation Functions (SIMD-optimized where possible)
    // ========================================================================
    double sum() const;
    double mean() const;
    double min() const;
    double max() const;
    double std() const;      // Standard deviation
    double var() const;      // Variance
    double median() const;
    double quantile(double q) const;  // q in [0,1]
    int64_t count() const;   // Non-null count
    
    // Rolling window operations
    Series rollingSum(size_t window) const;
    Series rollingMean(size_t window) const;
    Series rollingMin(size_t window) const;
    Series rollingMax(size_t window) const;
    Series rollingStd(size_t window) const;
    
    // Exponential weighted operations
    Series ewm(double alpha) const;  // Exponentially weighted mean
    
    // ========================================================================
    // Element-wise Operations (vectorized)
    // ========================================================================
    Series add(const Series& other) const;
    Series add(double scalar) const;
    Series sub(const Series& other) const;
    Series sub(double scalar) const;
    Series mul(const Series& other) const;
    Series mul(double scalar) const;
    Series div(const Series& other) const;
    Series div(double scalar) const;
    Series pow(double exponent) const;
    Series sqrt() const;
    Series abs() const;
    Series neg() const;
    Series log() const;
    Series log10() const;
    Series exp() const;
    Series sin() const;
    Series cos() const;
    Series tan() const;
    
    // Comparison operations (return bool series)
    Series eq(const Series& other) const;
    Series eq(double scalar) const;
    Series ne(const Series& other) const;
    Series ne(double scalar) const;
    Series lt(const Series& other) const;
    Series lt(double scalar) const;
    Series le(const Series& other) const;
    Series le(double scalar) const;
    Series gt(const Series& other) const;
    Series gt(double scalar) const;
    Series ge(const Series& other) const;
    Series ge(double scalar) const;
    
    // Logical operations (for bool series)
    Series logicalAnd(const Series& other) const;
    Series logicalOr(const Series& other) const;
    Series logicalNot() const;
    
    // String operations (for string series)
    Series strLen() const;
    Series strLower() const;
    Series strUpper() const;
    Series strContains(const std::string& pattern) const;
    Series strReplace(const std::string& from, const std::string& to) const;
    Series strSplit(const std::string& delimiter, int index) const;
    
    // ========================================================================
    // Transformations
    // ========================================================================
    Series shift(int periods) const;
    Series diff(int periods = 1) const;
    Series pctChange(int periods = 1) const;
    Series cumsum() const;
    Series cumprod() const;
    Series cummin() const;
    Series cummax() const;
    Series rank() const;
    Series normalize() const;  // (x - mean) / std
    Series minMaxScale(double newMin = 0.0, double newMax = 1.0) const;
    
    // ========================================================================
    // Filtering and Selection
    // ========================================================================
    Series head(size_t n = 5) const;
    Series tail(size_t n = 5) const;
    Series slice(size_t start, size_t end) const;
    Series take(const std::vector<size_t>& indices) const;
    Series where(const Series& condition) const;  // condition is bool series
    
    // ========================================================================
    // Sorting
    // ========================================================================
    Series sort(bool ascending = true) const;
    std::vector<size_t> argsort(bool ascending = true) const;
    std::vector<size_t> argmin(size_t n = 1) const;
    std::vector<size_t> argmax(size_t n = 1) const;
    
    // ========================================================================
    // Unique values
    // ========================================================================
    Series unique() const;
    size_t nunique() const;
    std::unordered_map<std::string, size_t> valueCounts() const;
    
    // ========================================================================
    // Type conversion
    // ========================================================================
    Series astype(DType newType) const;
    Series toDouble() const { return astype(DType::Float64); }
    Series toInt() const { return astype(DType::Int64); }
    Series toString() const { return astype(DType::String); }
    
    // ========================================================================
    // Copy and Clone
    // ========================================================================
    Series copy() const;
    
    // ========================================================================
    // Utility
    // ========================================================================
    std::string repr(size_t maxRows = 10) const;
    std::vector<double> toVector() const;  // Convert to double vector
    
private:
    DType dtype_;
    std::string name_;
    size_t size_ = 0;
    
    // Storage (only one is used based on dtype_)
    std::vector<double> float64_data_;
    std::vector<int64_t> int64_data_;
    std::vector<std::string> string_data_;
    std::vector<uint8_t> bool_data_;  // Packed bools
    
    // Null bitmap (bit i = 1 means element i is NOT null)
    std::vector<uint64_t> null_bitmap_;
    
    // Helper methods
    void initNullBitmap(size_t size);
    void setNullBit(size_t idx, bool isValid);
    bool getNullBit(size_t idx) const;
    
    // SIMD helpers
    static double simdSum(const double* data, size_t n);
    static void simdAdd(const double* a, const double* b, double* out, size_t n);
    static void simdMul(const double* a, const double* b, double* out, size_t n);
    static void simdScalarAdd(const double* a, double scalar, double* out, size_t n);
    static void simdScalarMul(const double* a, double scalar, double* out, size_t n);
};

// ============================================================================
// DataFrame: Collection of named Series (columns)
// ============================================================================

class DataFrame {
public:
    // Constructors
    DataFrame() = default;
    explicit DataFrame(const std::unordered_map<std::string, Series>& columns);
    
    // Static factory methods
    static DataFrame fromColumns(const std::vector<std::pair<std::string, Series>>& columns);
    static DataFrame fromRows(const std::vector<std::string>& columnNames,
                              const std::vector<std::vector<double>>& rows);
    static DataFrame fromCSV(const std::string& content, char delimiter = ',', bool hasHeader = true);
    
    // Basic info
    size_t numRows() const { return nrows_; }
    size_t numCols() const { return columns_.size(); }
    bool empty() const { return nrows_ == 0 || columns_.empty(); }
    std::vector<std::string> columnNames() const;
    std::vector<DType> dtypes() const;
    
    // Column access
    Series& operator[](const std::string& name);
    const Series& operator[](const std::string& name) const;
    Series& col(const std::string& name) { return (*this)[name]; }
    const Series& col(const std::string& name) const { return (*this)[name]; }
    bool hasColumn(const std::string& name) const;
    
    // Column management
    void addColumn(const std::string& name, const Series& series);
    void addColumn(const std::string& name, Series&& series);
    void removeColumn(const std::string& name);
    void renameColumn(const std::string& oldName, const std::string& newName);
    DataFrame select(const std::vector<std::string>& names) const;
    DataFrame drop(const std::vector<std::string>& names) const;
    
    // Row access
    std::unordered_map<std::string, double> rowAsMap(size_t idx) const;
    
    // ========================================================================
    // Filtering and Selection
    // ========================================================================
    DataFrame head(size_t n = 5) const;
    DataFrame tail(size_t n = 5) const;
    DataFrame slice(size_t start, size_t end) const;
    DataFrame take(const std::vector<size_t>& indices) const;
    DataFrame where(const Series& condition) const;
    DataFrame query(const std::string& expr) const;  // Simple query syntax
    
    // ========================================================================
    // Sorting
    // ========================================================================
    DataFrame sortBy(const std::string& column, bool ascending = true) const;
    DataFrame sortBy(const std::vector<std::string>& columns, 
                     const std::vector<bool>& ascending) const;
    
    // ========================================================================
    // Aggregation
    // ========================================================================
    std::unordered_map<std::string, double> sum() const;
    std::unordered_map<std::string, double> mean() const;
    std::unordered_map<std::string, double> min() const;
    std::unordered_map<std::string, double> max() const;
    std::unordered_map<std::string, double> std() const;
    std::unordered_map<std::string, double> var() const;
    std::unordered_map<std::string, int64_t> count() const;
    DataFrame describe() const;  // Summary statistics
    
    // ========================================================================
    // Group By Operations
    // ========================================================================
    class GroupBy {
    public:
        GroupBy(const DataFrame& df, const std::vector<std::string>& keys);
        
        DataFrame sum() const;
        DataFrame mean() const;
        DataFrame min() const;
        DataFrame max() const;
        DataFrame count() const;
        DataFrame first() const;
        DataFrame last() const;
        DataFrame agg(const std::unordered_map<std::string, std::string>& aggFuncs) const;
        
        size_t ngroups() const { return groups_.size(); }
        
    private:
        const DataFrame& df_;
        std::vector<std::string> keys_;
        std::unordered_map<std::string, std::vector<size_t>> groups_;
        
        void buildGroups();
        std::string makeGroupKey(size_t rowIdx) const;
    };
    
    GroupBy groupby(const std::string& column) const;
    GroupBy groupby(const std::vector<std::string>& columns) const;
    
    // ========================================================================
    // Join Operations
    // ========================================================================
    DataFrame merge(const DataFrame& other, const std::string& on,
                    const std::string& how = "inner") const;
    DataFrame merge(const DataFrame& other, 
                    const std::string& leftOn, const std::string& rightOn,
                    const std::string& how = "inner") const;
    DataFrame concat(const DataFrame& other, bool axis0 = true) const;  // axis0=rows, axis1=cols
    
    // ========================================================================
    // Transformations
    // ========================================================================
    DataFrame apply(const std::string& column, 
                    std::function<double(double)> func) const;
    DataFrame applyAll(std::function<double(double)> func) const;
    DataFrame transpose() const;
    DataFrame pivot(const std::string& index, const std::string& columns,
                    const std::string& values) const;
    DataFrame melt(const std::vector<std::string>& idVars,
                   const std::vector<std::string>& valueVars) const;
    
    // ========================================================================
    // Missing Data
    // ========================================================================
    DataFrame dropna(bool any = true) const;  // any=true: drop if any null; false: drop if all null
    DataFrame fillna(double value) const;
    DataFrame fillna(const std::unordered_map<std::string, double>& values) const;
    
    // ========================================================================
    // Correlation and Covariance
    // ========================================================================
    DataFrame corr() const;
    DataFrame cov() const;
    
    // ========================================================================
    // I/O
    // ========================================================================
    std::string toCSV(char delimiter = ',', bool includeHeader = true) const;
    std::string toJSON(bool orient_records = true) const;
    std::string repr(size_t maxRows = 10, size_t maxCols = 10) const;
    
    // ========================================================================
    // Copy
    // ========================================================================
    DataFrame copy() const;
    
    // ========================================================================
    // Memory info
    // ========================================================================
    size_t memoryUsage() const;
    
private:
    size_t nrows_ = 0;
    std::vector<std::string> column_order_;  // Preserve insertion order
    std::unordered_map<std::string, Series> columns_;
    
    void validateColumnSize(const Series& series) const;
    void updateRowCount();
};

// ============================================================================
// Global DataFrame Registry (for Quartz runtime integration)
// ============================================================================

class DataFrameRegistry {
public:
    static DataFrameRegistry& instance() {
        static DataFrameRegistry inst;
        return inst;
    }
    
    size_t store(DataFrame df);
    DataFrame* get(size_t id);
    const DataFrame* get(size_t id) const;
    void remove(size_t id);
    void clear();
    
    size_t storeSeries(Series series);
    Series* getSeries(size_t id);
    const Series* getSeries(size_t id) const;
    void removeSeries(size_t id);
    
private:
    DataFrameRegistry() = default;
    
    size_t nextDfId_ = 1;
    size_t nextSeriesId_ = 1;
    std::unordered_map<size_t, DataFrame> dataframes_;
    std::unordered_map<size_t, Series> series_;
};

}  // namespace qz_data

#endif  // DATAFRAME_H
