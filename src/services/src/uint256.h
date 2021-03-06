/*
 * This file is part of the Eccoin project
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2016 The Bitcoin Core developers
 * Copyright (c) 2014-2018 The Eccoin developers
 * Copyright (c) 2017-2019 The Xuez developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BITCOIN_UINT256_H
#define BITCOIN_UINT256_H

#include "common.h"
#include <assert.h>
#include <cstring>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>

/** Template base class for fixed-sized opaque blobs. */
template <unsigned int BITS>
class base_blob
{
protected:
    enum
    {
        WIDTH = BITS / 8
    };
    uint8_t data[WIDTH];

public:
    base_blob() { memset(data, 0, sizeof(data)); }
    explicit base_blob(const std::vector<unsigned char> &vch);

    bool IsNull() const
    {
        for (int i = 0; i < WIDTH; i++)
            if (data[i] != 0)
                return false;
        return true;
    }

    void SetNull() { memset(data, 0, sizeof(data)); }
    friend inline bool operator==(const base_blob &a, const base_blob &b)
    {
        return memcmp(a.data, b.data, sizeof(a.data)) == 0;
    }
    friend inline bool operator!=(const base_blob &a, const base_blob &b)
    {
        return memcmp(a.data, b.data, sizeof(a.data)) != 0;
    }
    friend inline bool operator<(const base_blob &a, const base_blob &b)
    {
        return memcmp(a.data, b.data, sizeof(a.data)) < 0;
    }

    std::string GetHex() const;
    void SetHex(const char *psz);
    void SetHex(const std::string &str);
    std::string ToString() const;

    unsigned char *begin() { return &data[0]; }
    unsigned char *end() { return &data[WIDTH]; }
    const unsigned char *begin() const { return &data[0]; }
    const unsigned char *end() const { return &data[WIDTH]; }
    unsigned int size() const { return sizeof(data); }
    uint64_t Get64(int n = 0) const { return data[2 * n] | (uint64_t)data[2 * n + 1] << 32; }
    unsigned int GetSerializeSize(int nType, int nVersion) const { return sizeof(data); }
    template <typename Stream>
    void Serialize(Stream &s) const
    {
        s.write((char *)data, sizeof(data));
    }

    template <typename Stream>
    void Unserialize(Stream &s)
    {
        s.read((char *)data, sizeof(data));
    }
};

/** 160-bit opaque blob.
 * @note This type is called uint160 for historical reasons only. It is an opaque
 * blob of 160 bits and has no integer operations.
 */
class uint160 : public base_blob<160>
{
public:
    uint160() {}
    uint160(const base_blob<160> &b) : base_blob<160>(b) {}
    explicit uint160(const std::vector<unsigned char> &vch) : base_blob<160>(vch) {}
};

/** 256-bit opaque blob.
 * @note This type is called uint256 for historical reasons only. It is an
 * opaque blob of 256 bits and has no integer operations. Use arith_uint256 if
 * those are required.
 */
class uint256 : public base_blob<256>
{
public:
    uint256() {}
    uint256(const base_blob<256> &b) : base_blob<256>(b) {}
    explicit uint256(const std::vector<unsigned char> &vch) : base_blob<256>(vch) {}
    /** A cheap hash function that just returns 64 bits from the result, it can be
     * used when the contents are considered uniformly random. It is not appropriate
     * when the value can easily be influenced from outside as e.g. a network adversary could
     * provide values to trigger worst-case behavior.
     */
    uint64_t GetCheapHash() const { return ReadLE64(data); }
    /** A more secure, salted hash function.
     * @note This hash is not stable between little and big endian.
     */
    uint64_t GetHash(const uint256 &salt) const;
};

/* uint256 from const char *.
 * This is a separate function because the constructor uint256(const char*) can result
 * in dangerously catching uint256(0).
 */
inline uint256 uint256S(const char *str)
{
    uint256 rv;
    rv.SetHex(str);
    return rv;
}
/* uint256 from std::string.
 * This is a separate function because the constructor uint256(const std::string &str) can result
 * in dangerously catching uint256(0) via std::string(const char*).
 */
inline uint256 uint256S(const std::string &str)
{
    uint256 rv;
    rv.SetHex(str);
    return rv;
}

#endif // BITCOIN_UINT256_H
