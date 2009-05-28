#ifndef __NULL_BW_MAP
#define __NULL_BW_MAP

#include "common/config.hpp"

/// Buffer wrapper map that offers no persistency
/**
   Implemented on top of cache_mt
*/

class null_bw_map {
    typedef cache_mt<buffer_wrapper, buffer_wrapper, config::lock_t, buffer_wrapper_hash, cache_mt_none<buffer_wrapper, buffer_wrapper_hash> > cache_t;

    cache_t *buffer_wrapper_cache;
    boost::uint64_t space_left;

public:
    null_bw_map(const std::string &db_name, boost::uint64_t cs, boost::uint64_t ts, unsigned int to);
    ~null_bw_map();

    bool read(const buffer_wrapper &key, buffer_wrapper *value);
    bool write(const buffer_wrapper &key, const buffer_wrapper &value);
    boost::uint64_t get_free();
};

#endif