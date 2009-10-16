#ifndef __SIMPLE_DHT
#define __SIMPLE_DHT

#include <openssl/sha.h>

#include "common/null_lock.hpp"
#include "common/config.hpp"
#include "rpc/rpc_client.hpp"
#include "provider/provider.hpp"

template <class SocketType>
class simple_dht {
public:
    // export the params
    typedef SocketType transport_t;
    typedef null_lock lock_t;
    typedef buffer_wrapper pkey_t;
    typedef buffer_wrapper pvalue_t;
    // TO DO: throw exceptions form DHT, capture them and modify to "void (const boost::system::error_code &, int)"
    typedef boost::function<void (int)> mutate_callback_t;
    typedef boost::function<void (pvalue_t)> get_callback_t;
    typedef rpc_client<SocketType> rpc_client_t;    
    typedef boost::shared_ptr<std::vector<get_callback_t> > get_callbacks_t;
    typedef boost::shared_ptr<std::vector<mutate_callback_t> > put_callbacks_t;

    simple_dht(boost::asio::io_service &io_service, unsigned int r = 0, unsigned int timeout = 10);
    ~simple_dht();
    void addGateway(const std::string &host, const std::string &service);
    void put(pkey_t key, pvalue_t value, int ttl, const std::string &secret, mutate_callback_t put_callback);    
    void get(pkey_t key, get_callback_t get_callback);
    void remove(pkey_t key, pvalue_t value, int ttl, const std::string &secret, mutate_callback_t remove_callback);
    void wait();

private:
    class gateway_t {
    public:
	std::string host, service;	
	rpcvector_t pending_gets, pending_puts;
	get_callbacks_t get_callbacks;
	put_callbacks_t put_callbacks;
	gateway_t(const std::string &h, const std::string &s) :
	    host(h), service(s), 
	    get_callbacks(new std::vector<get_callback_t>), put_callbacks(new std::vector<mutate_callback_t>) { }
    };

    rpc_client_t tp;
    std::vector<gateway_t> gateways;

    unsigned int choose_gateway(pkey_t key);
    void handle_put_callbacks(const simple_dht::put_callbacks_t &puts, const rpcreturn_t &error, const rpcvector_t &result);
    void handle_get_callbacks(const simple_dht::get_callbacks_t &gets, const rpcreturn_t &error, const rpcvector_t &result);
};

template <class SocketType>
void simple_dht<SocketType>::handle_put_callbacks(const simple_dht::put_callbacks_t &puts,
			  const rpcreturn_t &error,
			  const rpcvector_t &/*result*/) {
    for (unsigned int i = 0; i < puts->size(); i++)
	(*puts)[i](error);
}

template <class SocketType>
void simple_dht<SocketType>::handle_get_callbacks(const simple_dht::get_callbacks_t &gets,
						       const rpcreturn_t &error,
						       const rpcvector_t &result) {
    for (unsigned int i = 0; i < gets->size(); i++)
	if (i < result.size() && error == rpcstatus::ok)
	    (*gets)[i](result[i]);
	else
	    (*gets)[i](buffer_wrapper());
}

template <class SocketType>
void simple_dht<SocketType>::wait() {
    bool completed = false;

    while(!completed) {
	completed = true;
	for (unsigned int i = 0; i < gateways.size(); i++) {
	    if (gateways[i].pending_gets.size() > 0) {
		completed = false;
		get_callbacks_t callbacks(new std::vector<get_callback_t>);
		callbacks.swap(gateways[i].get_callbacks);
		tp.dispatch(gateways[i].host, gateways[i].service, 
			    PROVIDER_READ, gateways[i].pending_gets,
			    boost::bind(&simple_dht<SocketType>::handle_get_callbacks, this, callbacks, _1, _2));
		gateways[i].pending_gets.clear();
	    }
	    if (gateways[i].pending_puts.size() > 0) {
		completed = false;
		put_callbacks_t callbacks(new std::vector<mutate_callback_t>);
		callbacks.swap(gateways[i].put_callbacks);
		tp.dispatch(gateways[i].host, gateways[i].service, 
			    PROVIDER_WRITE, gateways[i].pending_puts,
			    boost::bind(&simple_dht<SocketType>::handle_put_callbacks, this, callbacks, _1, _2));
		gateways[i].pending_puts.clear();
	    }
	}
	tp.run();
    }
}

template <class SocketType>
simple_dht<SocketType>::simple_dht(boost::asio::io_service &io_service, unsigned int /*r*/, unsigned int /*t*/) : 
    tp(io_service) { }

template <class SocketType>
simple_dht<SocketType>::~simple_dht() { }

template <class SocketType>
void simple_dht<SocketType>::addGateway(const std::string &host, const std::string &service) {
    gateways.push_back(gateway_t(host, service));
}

template <class SocketType>
unsigned int simple_dht<SocketType>::choose_gateway(pkey_t key) {
    unsigned int hash = 0;
    unsigned char *k = (unsigned char *)key.get();

    // sdbm hashing used here
    for (unsigned int i = 0; i < key.size(); i++, k++)
	hash = *k + (hash << 6) + (hash << 16) - hash;
    
    return hash % gateways.size();
}

template <class SocketType>
void simple_dht<SocketType>::put(pkey_t key, pvalue_t value, int /*ttl*/, 
				      const std::string &/*secret*/, mutate_callback_t put_callback) {
    unsigned int index = choose_gateway(key);
    gateways[index].pending_puts.push_back(key);
    gateways[index].pending_puts.push_back(value);
    gateways[index].put_callbacks->push_back(put_callback);
}

template <class SocketType>
void simple_dht<SocketType>::remove(pkey_t /*key*/, pvalue_t /*value*/, int /*ttl*/, 
					 const std::string &/*secret*/, mutate_callback_t remove_callback) {
    // not yet implemented
    remove_callback(0);
}

template <class SocketType>
void simple_dht<SocketType>::get(pkey_t key, get_callback_t getvalue_callback) {
    unsigned int index = choose_gateway(key);
    gateways[index].pending_gets.push_back(key);
    gateways[index].get_callbacks->push_back(getvalue_callback);
}

#endif
