#pragma once

#include <cstdint>
#include <type_traits>
#include <concepts>
#include <limits>

namespace gel::fpga {
//FixedPoint<4,20>
//kullanilabilir optimum sonuc icin ama
template <int IntBits, int FracBits>
class FixedPoint {
private:
    static constexpr int TotalBits = IntBits + FracBits;
    
    using StorageType =
        std::conditional_t<TotalBits <= 8, int8_t,
        std::conditional_t<TotalBits <= 16, int16_t,
        std::conditional_t<TotalBits <= 32, int32_t,
        std::conditional_t<TotalBits <= 64, int64_t, void>>>>;
        

    #pragma HLS inline
    

    StorageType value;
    

    static constexpr StorageType SCALE = static_cast<StorageType>(1) << FracBits;
    
public:

    FixedPoint() : value(0) {}


    template <std::floating_point T>
    explicit FixedPoint(T v) : value(static_cast<StorageType>(v * SCALE)) {}


    template <std::integral T>
    explicit FixedPoint(T v) : value(static_cast<StorageType>(v * SCALE)) {}


    static FixedPoint from_raw(StorageType raw_value) {
        FixedPoint result;
        result.value = raw_value;
        return result;
    }


    operator float() const {
        return static_cast<float>(value) / SCALE;
    }


    operator double() const {
        return static_cast<double>(value) / SCALE;
    }



    double to_double() const {
        return static_cast<double>(value) / SCALE;
    }


    FixedPoint operator+(const FixedPoint& other) const {
        #pragma HLS inline
        FixedPoint result;
        result.value = value + other.value;
        return result;
    }
    
    FixedPoint operator-(const FixedPoint& other) const {
        #pragma HLS inline
        FixedPoint result;
        result.value = value - other.value;
        return result;
    }
    
    FixedPoint operator*(const FixedPoint& other) const {
        #pragma HLS inline
        FixedPoint result;
        auto wide_res = static_cast<int64_t>(value) *
                        static_cast<int64_t>(other.value);
        result.value = static_cast<StorageType>(wide_res >> FracBits);
        return result;
    }
    
    FixedPoint operator/(const FixedPoint& other) const {
        #pragma HLS inline
        FixedPoint result;
        if (other.value == 0) {
            result.value = (value >= 0) ?
                std::numeric_limits<StorageType>::max() :
                std::numeric_limits<StorageType>::min();
            return result;
        }
        auto scaled = static_cast<int64_t>(value) << FracBits;
        result.value = static_cast<StorageType>(scaled / other.value);
        return result;
    }

    bool operator==(const FixedPoint& other) const {
        #pragma HLS inline
        return value == other.value;
    }

    bool operator!=(const FixedPoint& other) const {
        #pragma HLS inline
        return value != other.value;
    }

    bool operator<(const FixedPoint& other) const {
        #pragma HLS inline
        return value < other.value;
    }

    bool operator>(const FixedPoint& other) const {
        #pragma HLS inline
        return value > other.value;
    }

    bool operator<=(const FixedPoint& other) const {
        #pragma HLS inline
        return value <= other.value;
    }

    bool operator>=(const FixedPoint& other) const {
        #pragma HLS inline
        return value >= other.value;
    }
};


struct Tick {
    #pragma HLS data_pack variable=Tick
    
    uint64_t timestamp;
    
    FixedPoint<10, 6> bid;
    FixedPoint<10, 6> ask;
    
    uint32_t volume;
};

} // namespace gel::fpga