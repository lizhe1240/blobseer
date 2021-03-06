#ifndef __RPC_META
#define __RPC_META

#include <deque>
#include <sstream>

#include <boost/variant.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>

#include "common/buffer_wrapper.hpp"
#include "rpc/rpc_sync_socket.hpp"

// Basic types
typedef std::pair <std::string, std::string> string_pair_t;
typedef std::vector<buffer_wrapper> rpcvector_t;
typedef boost::shared_ptr<rpcvector_t> prpcvector_t;
namespace rpcstatus {
    typedef boost::int32_t rpcreturn_t;
    const rpcreturn_t ok = 0, earg = 7, eres = 28, eobj = 6;
}
typedef rpcstatus::rpcreturn_t rpcreturn_t;

// Callbacks
typedef boost::function<void (const rpcreturn_t &, const rpcvector_t &) > rpcclient_callback_t;
typedef boost::function<rpcreturn_t (const rpcvector_t &, rpcvector_t &)> rpcserver_callback_t;
typedef boost::function<rpcreturn_t (const rpcvector_t &, rpcvector_t &, const std::string &sender)> rpcserver_extcallback_t;

typedef boost::variant<rpcclient_callback_t, rpcserver_callback_t, rpcserver_extcallback_t> callback_t;

/// RPC header: RPC name (id), number of parameters, status code
class rpcheader_t {
public:
    boost::uint32_t name, psize;
    // unsigned int name, psize;
    boost::int32_t status;
    
    rpcheader_t(boost::uint32_t n, boost::uint32_t s) : name(n), psize(s), status(rpcstatus::ok) { }
};

template<class SocketType> class rpcinfo_t : public boost::static_visitor<rpcstatus::rpcreturn_t>, private boost::noncopyable {
public:
    typedef rpc_sync_socket<SocketType> socket_t;
    typedef boost::shared_ptr<socket_t> psocket_t;

    static const boost::uint32_t RPC_TIMEOUT = 10;

    boost::int32_t id;
    string_pair_t host_id;
    rpcvector_t params;
    rpcvector_t result;
    rpcheader_t header;
    psocket_t socket;
    callback_t callback;
        
    rpcinfo_t(boost::asio::io_service &io) : header(rpcheader_t(0, 0)), socket(new socket_t(io)) { }
    template<class Callback> rpcinfo_t(const std::string &h, const std::string &s,
				       boost::uint32_t n, const rpcvector_t &p, 
				       Callback c, const rpcvector_t &r) : 
	id(0), host_id(string_pair_t(h, s)), params(p), 
	result(r), header(rpcheader_t(n, p.size())), callback(c) {
    }
	
    void assign_id(const boost::int32_t request_id) {
	id = request_id;
    }
    
    rpcstatus::rpcreturn_t operator()(const rpcclient_callback_t &cb) {
	cb(rpcreturn_t(header.status), result);
	return rpcstatus::ok;
    }

    rpcstatus::rpcreturn_t operator()(const rpcserver_callback_t &cb) {
	return cb(params, result);
    }
    
    rpcstatus::rpcreturn_t operator()(const rpcserver_extcallback_t &cb) {
	std::stringstream out;
	out << socket->socket().remote_endpoint().address().to_string() << ":" << socket->socket().remote_endpoint().port();
	return cb(params, result, out.str());
    }
};

#endif
