/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host test stand-in for xiaozhi's Settings: an in-memory NVS that counts its writes
#pragma once
#include <map>
#include <string>

struct HostNvs {
    std::map<std::string, int> ints;
    int writes = 0;
};

inline HostNvs& GetHostNvs()
{
    static HostNvs nvs;
    return nvs;
}

class Settings {
public:
    Settings(const std::string& ns, bool readWrite) : _ns(ns), _read_write(readWrite) {}
    int GetInt(const std::string& key, int defaultValue)
    {
        const auto it = GetHostNvs().ints.find(_ns + "/" + key);
        return it == GetHostNvs().ints.end() ? defaultValue : it->second;
    }
    void SetInt(const std::string& key, int value)
    {
        if (_read_write) {
            GetHostNvs().ints[_ns + "/" + key] = value;
            GetHostNvs().writes++;
        }
    }

private:
    std::string _ns;
    bool _read_write;
};
