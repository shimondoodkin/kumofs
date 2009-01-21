#ifndef LOGPACKER_H__
#define LOGPACKER_H__

#include "logpack.hpp"
#include <memory>

class logpacker {
public:
	static void initialize(const std::string& basename, size_t lotate_size);
	static void destroy();
	bool is_active() { return !!s_instance.get(); }
	logpack& instance() { return *s_instance; }
private:
	static std::auto_ptr<logpack> s_instance;
};

#define LOGPACK(name, version, ...) \
	do { \
		if(logpacker::is_active()) { \
			logpacker::instance().write(name, version, __VA_ARGS__); \
		} \
	} while(0)

#endif /* logpacker.h */
