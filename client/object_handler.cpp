#include <sstream>
#include <libconfig.h++>
#include <openssl/md5.h>

#include "pmanager/publisher.hpp"
#include "vmanager/main.hpp"
#include "object_handler.hpp"
#include "replica_policy.hpp"

#include "common/debug.hpp"

using namespace std;

const unsigned int HASH_SIZE = 16;

object_handler::object_handler(const std::string &config_file) : latest_root(0, 0, 0, 0, 0) {
    libconfig::Config cfg;
    
    try {
        cfg.readFile(config_file.c_str());
	// get dht port
	std::string service;
	if (!cfg.lookupValue("dht.service", service))
	    FATAL("DHT port is missing/invalid");
	// get dht gateways
	libconfig::Setting &s = cfg.lookup("dht.gateways");
	int ng = s.getLength();
	if (!s.isList() || ng <= 0) 
	    FATAL("Gateways are missing/invalid");
	// get dht parameters
	int replication, timeout, cache_size;
	if (!cfg.lookupValue("dht.replication", replication) ||
	    !cfg.lookupValue("dht.timeout", timeout) ||
	    !cfg.lookupValue("dht.cachesize", cache_size))
	    FATAL("DHT parameters are missing/invalid");
	// build dht structure
	dht = new dht_t(io_service, replication, timeout, cache_size);
	for (int i = 0; i < ng; i++) {
	    std::string stmp = s[i];
	    dht->addGateway(stmp, service);
	}
	// get provider parameters
	if (!cfg.lookupValue("provider.retry", retry_count))
	    FATAL("Provider retry count is missing/invalid");
	if (!cfg.lookupValue("provider.deduplication", dedup_flag))
	    FATAL("Provider deduplication flag is missing/invalid");
	// get other parameters
	if (!cfg.lookupValue("pmanager.host", publisher_host) ||
	    !cfg.lookupValue("pmanager.service", publisher_service) ||
	    !cfg.lookupValue("vmanager.host", vmgr_host) ||
	    !cfg.lookupValue("vmanager.service", vmgr_service))
	    FATAL("pmanager/vmanager host/service are missing/invalid");
	// complete object creation
	query = new interval_range_query(dht, dedup_flag);
	direct_rpc = new rpc_client_t(io_service);
    } catch(libconfig::FileIOException) {
	FATAL("I/O error trying to parse config file: " + config_file);
    } catch(libconfig::ParseException &e) {
	std::ostringstream ss;
	ss << "parse exception for cfg file " << config_file 
	   << "(line " << e.getLine() << "): " << e.getError();
	FATAL(ss.str());
    } catch(std::runtime_error &e) {
	throw e;
    } catch(...) {
	FATAL("unexpected exception");
    }

    // set the random number generator seed
    rnd.seed(boost::posix_time::microsec_clock::local_time().time_of_day().total_nanoseconds());
        
    DBG("constructor init complete");
}

object_handler::~object_handler() {
    delete direct_rpc;
    delete query;
    delete dht;
}

void object_handler::rpc_provider_callback(boost::int32_t call_type, buffer_wrapper page_key, 
                                           interval_range_query::replica_policy_t &repl, 
                                           buffer_wrapper buffer, bool &result, 
					   unsigned int retries,
                                           const rpcreturn_t &error, const rpcvector_t &val) {
    if (error == rpcstatus::ok && val.size() == 1)
        return;
    
    metadata::provider_desc adv = repl.try_next();
    if (adv.empty()) {
        adv = repl.try_again();
        retries++;
    }
    if (retries == retry_count) {
        INFO("could not fetch page: " << page_key << ", error is " << error 
             << "; no more replicas and/or retries left - ABORTED");
        result = false;
        return;
    }
    rpcvector_t read_params;
    read_params.push_back(page_key);
    INFO("could not fetch page: " << page_key << ", error is " << error 
         << "; next try is from: " << adv);
    direct_rpc->dispatch(adv.host, adv.service, call_type, read_params,
                         boost::bind(&object_handler::rpc_provider_callback, this, 
                                     call_type, page_key, 
                                     boost::ref(repl), buffer, boost::ref(result), 
				     retries, _1, _2),
                         rpcvector_t(1, buffer));
}

void object_handler::rpc_pagekey_callback(boost::dynamic_bitset<> &res, 
					  const metadata::replica_list_t &adv,
					  buffer_wrapper key, buffer_wrapper value,
					  unsigned int j, unsigned int replica_count,
					  buffer_wrapper providers) {
    if (providers == buffer_wrapper()) {
	rpcvector_t write_params;
	write_params.push_back(key);
	write_params.push_back(value);

	for (unsigned int k = j; k < j + replica_count; k++)
	    direct_rpc->dispatch(adv[k].host, adv[k].service, PROVIDER_WRITE, write_params,
				 boost::bind(&object_handler::rpc_write_callback, this,
					     boost::ref(res), boost::cref(adv[k]),
					     key, value, k, 0, _1, _2));
    } else {
	for (unsigned int k = j; k < j + replica_count; k++)
	    res[k] = true;
	INFO("a copy page " << key << " already exists; supressing duplicate");
    }
}
    
void object_handler::rpc_write_callback(boost::dynamic_bitset<> &res, 
					const metadata::provider_desc &adv,
					buffer_wrapper key, buffer_wrapper value,
					unsigned int k, unsigned int retries,
					const rpcreturn_t &error, const rpcvector_t &) {
    res[k] = (error == rpcstatus::ok);
    if (res[k])
	return;

    INFO("a replica of page " << k << " could not be written successfully, error is: " 
	 << error);
    if (retries == retry_count)
	return;

    rpcvector_t write_params;
    write_params.push_back(key);
    write_params.push_back(value);
    direct_rpc->dispatch(adv.host, adv.service, PROVIDER_WRITE, write_params,
			 boost::bind(&object_handler::rpc_write_callback, this,
				     boost::ref(res), boost::cref(adv),
				     key, value, k, retries + 1, _1, _2));
}

void object_handler::rpc_result_callback(bool &res, const rpcreturn_t &error, 
					 const rpcvector_t &) {
    if (error != rpcstatus::ok) {
	res = false;
	ERROR("could not perform RPC successfully, RPC status is: " << error);
    }
}

template <class T> static void rpc_get_serialized(bool &res, T &output, 
						  const rpcreturn_t &error, 
						  const rpcvector_t &result) {
    if (error == rpcstatus::ok && result.size() == 1 && result[0].getValue(&output, true))
	return;
    res = false;
    ERROR("could not perform RPC successfully; RPC status is: " << error);
}

bool object_handler::get_root(boost::uint32_t version, metadata::root_t &root) {
    bool result = true;

    if (version == 0 || !version_cache.read(version, &root)) {
	rpcvector_t params;
	params.push_back(buffer_wrapper(id, true));
	params.push_back(buffer_wrapper(version, true));
	direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_GETROOT, params,
			     bind(&rpc_get_serialized<metadata::root_t>, boost::ref(result), 
				  boost::ref(root), _1, _2));
	direct_rpc->run();
	if (result && version != 0)
	    version_cache.write(root.node.version, root);
    }

    return result;
}

bool object_handler::get_locations(page_locations_t &loc, boost::uint64_t offset, boost::uint64_t size, 
				   boost::uint32_t version) {
    metadata::root_t query_root(0, 0, 0, 0, 0);

    if (version == 0)
	query_root = latest_root;
    else
	if (!get_root(version, query_root))
	    return false;

    DBG("query root = " << query_root.node <<  ", version = " << version);

    if (query_root.node.version < version)
	throw std::runtime_error("object_handler::get_locations(): requested version higher than latest available version");
    if (query_root.page_size == 0)
	throw std::runtime_error("object_handler::get_locations(): read attempt on unallocated/uninitialized object");
    if (offset + size > query_root.node.size)
	throw std::runtime_error("object_handler::get_locations(): read attempt beyond maximal size");
    
    TIMER_START(read_locations);
    boost::uint64_t psize = query_root.page_size;
    boost::uint64_t new_offset = (offset / psize) * psize;
    boost::uint64_t nbr_vadv = (size + offset - new_offset) / psize + (((offset + size) % psize == 0) ? 0 : 1);
    boost::uint64_t new_size = nbr_vadv * psize;
    std::vector<random_select> vadv(nbr_vadv);

    metadata::query_t range(query_root.node.id, query_root.node.version, new_offset, new_size);
    blob::prefetch_list_t unused;

    TIMER_START(meta_timer);
    bool result = query->readRecordLocations(vadv, unused, range, query_root, 0xFFFFFFFF);
    TIMER_STOP(meta_timer, "GET_LOCATIONS " << range << ": Metadata read operation, success: " << result);
    if (!result)
	return false;

    metadata::provider_desc adv;
    for (boost::uint64_t i = 0; i < vadv.size(); i++)
	while (1) {
	    metadata::provider_desc adv = vadv[i].try_next();
	    if (adv.empty()) 
		break;
	    loc.push_back(page_location_t(adv, i * query_root.page_size, query_root.page_size));
	}
    TIMER_STOP(read_locations, "GET_LOCATIONS " << range 
	       << ": Page location vector has been successfully constructed");

    return true;
}

bool object_handler::read(boost::uint64_t offset, boost::uint64_t size, char *buffer, 
			  boost::uint32_t version, boost::uint32_t threshold,
			  const blob::prefetch_list_t &prefetch_list) {
    metadata::root_t query_root(0, 0, 0, 0, 0);

    if (version == 0)
	query_root = latest_root;
    else
	if (!get_root(version, query_root))
	    return false;

    DBG("query root = " << query_root.node <<  ", version = " << version);

    if (query_root.node.version < version)
	throw std::runtime_error("object_handler::read(): read attempt on a version higher than latest available version");
    if (query_root.page_size == 0)
	throw std::runtime_error("object_handler::read(): read attempt on unallocated/uninitialized object");
    if (offset + size > query_root.node.size)
	throw std::runtime_error("object_handler::read(): read attempt beyond maximal size");

    TIMER_START(read_timer);
    boost::uint64_t psize = query_root.page_size;
    boost::uint64_t new_offset = (offset / psize) * psize;
    boost::uint64_t nbr_vadv = (size + offset - new_offset) / psize + (((offset + size) % psize == 0) ? 0 : 1);
    boost::uint64_t new_size = nbr_vadv * psize;
    std::vector<random_select> vadv(nbr_vadv);

    metadata::query_t range(query_root.node.id, query_root.node.version, new_offset, new_size);

    TIMER_START(meta_timer);
    // !! BAD practice to const_cast. Interface needs to be redesigned.
    bool result = query->readRecordLocations(vadv, const_cast<blob::prefetch_list_t &>(prefetch_list), 
					     range, query_root, threshold);
    TIMER_STOP(meta_timer, "READ " << range << ": Metadata read operation, success: " << result);
    if (!result)
	return false;

    rpcvector_t read_params;
    TIMER_START(data_timer);
    
    // size of partial read from leftmost/rightmost page when read operation is unaligned
    uint64_t left_part = (query_root.page_size - (offset % query_root.page_size)) % query_root.page_size;
    uint64_t right_part = (offset + size) % query_root.page_size;

    buffer_wrapper left_buffer, right_buffer;

    // if the left end of range is unaligned, read only the needed part of the page involved
    unsigned int l = 0;
    if (offset % psize != 0) {
	l++;
	DBG("UNALIGNED LEFT READ QUERY " << vadv[0].get_page_key());
	read_params.clear();
	read_params.push_back(vadv[0].get_page_key());
	read_params.push_back(buffer_wrapper(psize - left_part, true));
	if (left_part < size) {
	    read_params.push_back(buffer_wrapper(left_part, true));
	    left_buffer = buffer_wrapper(buffer, left_part, true);
	} else {
	    read_params.push_back(buffer_wrapper(size, true));
	    left_buffer = buffer_wrapper(buffer, size, true);
	}
	metadata::provider_desc adv = vadv[0].try_next();
	if (adv.empty())
	    return false;

	direct_rpc->dispatch(adv.host, adv.service, PROVIDER_READ_PARTIAL, read_params,
			     boost::bind(&object_handler::rpc_provider_callback, this, 
					 PROVIDER_READ_PARTIAL, read_params.back(), 
					 boost::ref(vadv[0]), left_buffer, 
					 boost::ref(result), 0, _1, _2), 
			     rpcvector_t(1, left_buffer));
    }

    // read the whole rightmost page to a temporary buffer if the right end of range is unaligned
    unsigned int r = vadv.size();
    if (((offset + size) % psize != 0 && r > 1) || (offset % psize == 0 && size < psize)) {
	r--;
	DBG("UNALIGNED RIGHT READ QUERY " << vadv[r].get_page_key());
	read_params.clear();
	read_params.push_back(vadv[r].get_page_key());
	read_params.push_back(buffer_wrapper((boost::uint64_t)0, true));
	read_params.push_back(buffer_wrapper(right_part, true));
	right_buffer = buffer_wrapper(buffer + left_part + (r - l) * psize, right_part, true);
	metadata::provider_desc adv = vadv[r].try_next();
	if (adv.empty())
	    return false;
	direct_rpc->dispatch(adv.host, adv.service, PROVIDER_READ_PARTIAL, read_params,
			     boost::bind(&object_handler::rpc_provider_callback, this,
					 PROVIDER_READ_PARTIAL, read_params.back(),
					 boost::ref(vadv[r]), right_buffer, 
					 boost::ref(result), 0, _1, _2),
			     rpcvector_t(1, right_buffer));
    }
    
    // read all aligned pages directly in the user-supplied buffer
    for (unsigned int i = l; result && i < r; i++) {
	DBG("FULL READ QUERY " << vadv[i].get_page_key());
	read_params.clear();
	read_params.push_back(vadv[i].get_page_key());
	metadata::provider_desc adv = vadv[i].try_next();
	if (adv.empty())
	    return false;

	buffer_wrapper wr_buffer(buffer + left_part + (i - l) * query_root.page_size, 
				 query_root.page_size, true);

	direct_rpc->dispatch(adv.host, adv.service, PROVIDER_READ, read_params,
			     boost::bind(&object_handler::rpc_provider_callback, this, 
					 PROVIDER_READ, read_params.back(), 
					 boost::ref(vadv[i]), wr_buffer, 
					 boost::ref(result), 0, _1, _2),
			     rpcvector_t(1, wr_buffer));
    }
    direct_rpc->run();
    TIMER_STOP(data_timer, "READ " << range << ": Data read operation, success: " << result);

    TIMER_STOP(read_timer, "READ " << range << ": has completed");
    return result;
}

boost::uint32_t object_handler::append(boost::uint64_t size, char *buffer) {
    return exec_write(0, size, buffer, true);
}

boost::uint32_t object_handler::write(boost::uint64_t offset, boost::uint64_t size, 
				      char *buffer) {
    return exec_write(offset, size, buffer, false);
}

boost::uint32_t object_handler::exec_write(boost::uint64_t offset, boost::uint64_t size, 
					   char *buffer, bool append) {
    if (latest_root.page_size == 0)
	throw std::runtime_error("object_handler::write(): write attempt on unallocated/uninitialized object");
    ASSERT(offset % latest_root.page_size == 0 && size % latest_root.page_size == 0);

    TIMER_START(write_timer);
    bool result = true;

    metadata::query_t range(id, rnd(), offset, size);
    boost::uint64_t page_size = latest_root.page_size;
    unsigned int replica_count = latest_root.replica_count;

    metadata::replica_list_t adv;
    std::vector<buffer_wrapper> leaf_keys;
    rpcvector_t params;

    // try to get a list of providers
    TIMER_START(publisher_timer);
    params.clear();
    params.push_back(buffer_wrapper((size / page_size) * replica_count, true));
    params.push_back(buffer_wrapper(replica_count, true));
    direct_rpc->dispatch(publisher_host, publisher_service, PUBLISHER_GET, params,
			 boost::bind(&rpc_get_serialized<metadata::replica_list_t>, boost::ref(result), 
				     boost::ref(adv), _1, _2));
    direct_rpc->run();
    TIMER_STOP(publisher_timer, "WRITE " << range << ": PUBLISHER_GET, result: " << result);
    if (!result || adv.size() < (size / page_size) * replica_count)
	return 0;

    // write the set of pages to the page providers
    boost::dynamic_bitset<> page_results(adv.size());
    TIMER_START(providers_timer);
    for (boost::uint64_t i = 0, j = 0; i * page_size < size; i++, j += replica_count) {
	// prepare the page
	buffer_wrapper page_contents(buffer + i * page_size, page_size, true);
	char *hash = new char[HASH_SIZE];
	MD5((unsigned char *)page_contents.get(), page_contents.size(), (unsigned char *)hash);
	buffer_wrapper page_key(hash, HASH_SIZE);

	if (dedup_flag) {
	    unsigned int k;

	    for (k = 0; k < leaf_keys.size(); k++)
		if (memcmp(leaf_keys[k].get(), page_key.get(), HASH_SIZE) == 0) {
		    INFO("a copy page " << page_key << " already exists; supressing duplicate");
		    break;
		}
	    if (k == leaf_keys.size())
		dht->get(page_key, 
			 boost::bind(&object_handler::rpc_pagekey_callback, this, 
				     boost::ref(page_results), boost::cref(adv),
				     page_key, page_contents, j, replica_count, _1));
	    else
		for (k = j; k < j + replica_count; k++)
		    page_results[k] = true;
	} else {
	    rpcvector_t write_params;
	    write_params.push_back(page_key);
	    write_params.push_back(page_contents);

	    // write the replicas
	    for (unsigned int k = j; k < j + replica_count; k++)
		direct_rpc->dispatch(adv[k].host, adv[k].service, PROVIDER_WRITE,
				     write_params,		     
				     boost::bind(&object_handler::rpc_write_callback, this,
						 boost::ref(page_results), boost::cref(adv[k]),
						 page_key, page_contents, k, 0, 
						 _1, _2));
	}
	leaf_keys.push_back(page_key);
    }
    dht->wait();
    direct_rpc->run();
    
    // make sure each page has at least one successfully written replica
    for (unsigned int i = 0; i < adv.size() && result; i += replica_count) {
	unsigned int k;
	for (k = i; k < i + replica_count; k++)
	    if (page_results[k])
		break;
	if (k == i + replica_count) {
	    ERROR("WRITE " << range << ": none of the replicas of page " << i / replica_count 
		  << " could be written successfully, aborted");
	    result = false;
	    break;
	}
    }	
    TIMER_STOP(providers_timer, "WRITE " << range 
	       << ": Data written to providers, result: " << result);
    if (!result)
	return 0;
    
    // get a ticket from the version manager
    params.clear();
    params.push_back(buffer_wrapper(range, true));
    params.push_back(buffer_wrapper(append, true));

    vmgr_reply mgr_reply;
    TIMER_START(ticket_timer);
    direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_GETTICKET, params,
			 bind(&rpc_get_serialized<vmgr_reply>, boost::ref(result),
			      boost::ref(mgr_reply), _1, _2));
    direct_rpc->run();
    TIMER_STOP(ticket_timer, "WRITE " << range << ": VMGR_GETTICKET, result: " << result);
    if (!result)
	return 0;

    // construct the set of leaves to be written to the metadata
    range = mgr_reply.intervals.rbegin()->first;
    TIMER_START(metadata_timer);
    result = query->writeRecordLocations(mgr_reply, leaf_keys, adv);
    TIMER_STOP(metadata_timer, "WRITE " << range << ": writeRecordLocations(), result: " 
	       << result);
    if (!result)
	return 0;

    // publish the latest written version
    TIMER_START(publish_timer);
    params.clear();
    params.push_back(buffer_wrapper(range, true));
    direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_PUBLISH, params,
			 boost::bind(&object_handler::rpc_result_callback, this,
				     boost::ref(result), _1, _2));
    direct_rpc->run();
    TIMER_STOP(publish_timer, "WRITE " << range << ": VMGR_PUBLISH, result: " << result);
    TIMER_STOP(write_timer, "WRITE " << range << ": has completed, result: " << result);
    
    return result ? range.version : 0;
}

bool object_handler::create(boost::uint64_t page_size, boost::uint32_t replica_count) {
    bool result = true;

    rpcvector_t params;
    params.push_back(buffer_wrapper(page_size, true));
    params.push_back(buffer_wrapper(replica_count, true));
    direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_CREATE, params,
			 boost::bind(rpc_get_serialized<metadata::root_t>, boost::ref(result), 
				     boost::ref(latest_root), _1, _2));
    direct_rpc->run();
    id = latest_root.node.id;
    DBG("create result = " << result << ", id = " << id);
    return result;
}

bool object_handler::get_latest(boost::uint32_t id_) {
    if (id_ != 0)
	id = id_;

    bool result = get_root(0, latest_root);
    DBG("latest version request: " << latest_root.node);
    return result;
}

boost::int32_t object_handler::get_objcount() const {
    bool result = true;
    boost::int32_t obj_no;

    rpcvector_t params;    
    direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_GETOBJNO, rpcvector_t(), 
			 boost::bind(rpc_get_serialized<boost::int32_t>, boost::ref(result), 
				     boost::ref(obj_no), _1, _2));
    direct_rpc->run();
    DBG("the total number of blobs is: " << obj_no);
    if (result)
	return obj_no;
    else
	return -1;
}

bool object_handler::clone(boost::int32_t id_, boost::int32_t version_) {
    metadata::root_t clone_root(0, 0, 0, 0, 0);
    bool result = true;
    if (id_ == 0)
	id_ = id;

    rpcvector_t params;
    params.push_back(buffer_wrapper(id_, true));
    params.push_back(buffer_wrapper(version_, true));
    direct_rpc->dispatch(vmgr_host, vmgr_service, VMGR_CLONE, params, 
			 boost::bind(rpc_get_serialized<metadata::root_t>, boost::ref(result), 
				     boost::ref(clone_root), _1, _2));
    direct_rpc->run();
    DBG("new clone: " << clone_root.node);
    if (result && !clone_root.empty()) {
	latest_root = clone_root;
	latest_root.node.id = id;
	id = clone_root.node.id;
	
	return true;
    } else
	return false;
}
