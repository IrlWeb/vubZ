#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace obf_detail {
    inline constexpr uint64_t BuildSeed() {
        uint64_t h = 0xcbf29ce484222325ULL;

        for (const char c : __TIME__) {
            h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            h *= 0x100000001b3ULL;
        }

        return h;
    }

    inline constexpr uint8_t KeyByte(uint64_t seed, size_t i) {
        uint64_t z =
            seed + (static_cast<uint64_t>(i) + 1ULL) *
            0x9E3779B97F4A7C15ULL;

        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;

        return static_cast<uint8_t>(
            z ^ (z >> 8) ^ (z >> 16) ^ (z >> 24) ^
            (z >> 32) ^ (z >> 40) ^ (z >> 48) ^ (z >> 56)
            );
    }

    inline constexpr char Transform(char c, uint64_t seed, size_t i) {
        return static_cast<char>(
            static_cast<uint8_t>(c) ^ KeyByte(seed, i)
            );
    }
}

template <size_t N, uint64_t Seed>
class ObfString {
    static_assert(N > 0, "ObfString requires a string literal");

private:
    std::array<uint8_t, N> encrypted_{};

public:
    constexpr explicit ObfString(const char(&str)[N]) noexcept {
        for (size_t i = 0; i < N; ++i) {
            encrypted_[i] = static_cast<uint8_t>(
                static_cast<uint8_t>(str[i]) ^
                obf_detail::KeyByte(Seed, i)
                );
        }
    }

    [[nodiscard]] std::string decrypt() const {
        std::string result(N - 1, '\0');

        for (size_t i = 0; i < N - 1; ++i) {
            result[i] = static_cast<char>(
                encrypted_[i] ^ obf_detail::KeyByte(Seed, i)
                );
        }

        return result;
    }

    [[nodiscard]] std::array<char, N> decrypt_array() const noexcept {
        std::array<char, N> result{};

        for (size_t i = 0; i < N; ++i) {
            result[i] = static_cast<char>(
                encrypted_[i] ^ obf_detail::KeyByte(Seed, i)
                );
        }

        result[N - 1] = '\0';
        return result;
    }
};

#define SUPQ_OBF_IMPL(str, counter)                                        \
    ([]() -> std::string {                                                 \
        constexpr uint64_t seed =                                          \
            obf_detail::BuildSeed() +                                      \
            static_cast<uint64_t>(counter) * 0x9E3779B97F4A7C15ULL;        \
        constexpr ObfString<sizeof(str), seed> value(str);                  \
        return value.decrypt();                                            \
    }())

#define SUPQ_OBF(str) SUPQ_OBF_IMPL(str, __COUNTER__)
#define SUPQ2_OBF(str) SUPQ_OBF(str).c_str()

#define NAM_OBF(str) SUPQ_OBF(str)
#define NAM2_OBF(str) SUPQ2_OBF(str)
