#ifndef OBJECT_POOL
#define OBJECT_POOL

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/unordered_map.hpp>

template <class Object>
class object_pool_t {
private:
    static const unsigned int DEFAULT_POOL_SIZE = 16;

public:
    typedef Object object_t;
    typedef boost::shared_ptr<object_t> pobject_t;

    object_pool_t(boost::function<pobject_t()> _gen,
		  unsigned int size = DEFAULT_POOL_SIZE);
    pobject_t acquire();
    void release(pobject_t object);

private:
    boost::mutex map_lock;
    typedef std::pair<pobject_t, bool> map_entry_t;
    typedef boost::unordered_map<pobject_t, bool> object_map_t;
    object_map_t object_map;
    boost::function<pobject_t()> gen;
    unsigned int pool_size;
};

template <class Object>
object_pool_t<Object>::object_pool_t(boost::function<pobject_t()> _gen, unsigned int size) :
    gen(_gen), pool_size(size) { 
}

template <class Object>
typename object_pool_t<Object>::pobject_t object_pool_t<Object>::acquire() {
    boost::mutex::scoped_lock lock(map_lock);

    typename object_map_t::iterator i = object_map.begin();
    while (i != object_map.end() && i->second)
	i++;
    if (i != object_map.end()) {
	i->second = true;
	return i->first;
    } else if (object_map.size() < pool_size) {
	pobject_t result = gen();
	object_map.insert(map_entry_t(result, true));
	return result;
    } else
	return pobject_t();
}

template <class Object>
void object_pool_t<Object>::release(pobject_t object) {
    boost::mutex::scoped_lock lock(map_lock);

    typename object_map_t::iterator i = object_map.find(object);
    if (i != object_map.end())
	i->second = false;
    else
	throw std::runtime_error("object_pool_t::release(): object is not in the pool");
}

#endif
