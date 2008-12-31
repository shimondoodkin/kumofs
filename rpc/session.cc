#include "rpc/rpc.h"
#include "rpc/protocol.h"
#include "log/mlogger.h" //FIXME
#include <iterator>

namespace rpc {


basic_session::~basic_session()
{
	// FIXME
	//if(mp::iothreads::is_end()) { return; }

	msgpack::object res;
	res.type = msgpack::type::NIL;
	msgpack::object err;
	err.type = msgpack::type::POSITIVE_INTEGER;
	err.via.u64 = protocol::TRANSPORT_LOST_ERROR;

	force_lost(res, err);
}

session::~session() { }


void basic_session::process_response(
		basic_shared_session& self,
		msgobj result, msgobj error,
		msgid_t msgid, auto_zone z)
{
	pthread_scoped_lock lk(m_callbacks_mutex);

	callbacks_t::iterator it(m_callbacks.find(msgid));
	LOG_DEBUG("process callback this=",(void*)this," id=",msgid," found:",(it != m_callbacks.end())," result:",result," error:",error);
	if(it == m_callbacks.end()) { return; }

	callback_entry e = it->second;
	m_callbacks.erase(it);

	lk.unlock();
	e.callback(self, result, error, z);
}


void basic_session::send_data(const char* buf, size_t buflen,
		void (*finalize)(void*), void* data)
{
	pthread_scoped_lock lk(m_binds_mutex);
	if(m_binds.empty()) {
		throw std::runtime_error("session not bound");
	}
	// ad-hoc load balancing
	m_binds[m_msgid_rr % m_binds.size()]->send_data(
			buf, buflen, finalize, data);
}

void basic_session::send_datav(vrefbuffer* buf,
		void (*finalize)(void*), void* data)
{
	pthread_scoped_lock lk(m_binds_mutex);
	if(m_binds.empty()) {
		throw std::runtime_error("session not bound");
	}
	// ad-hoc load balancing
	m_binds[m_msgid_rr % m_binds.size()]->send_datav(
			buf, finalize, data);
}


bool basic_session::bind_transport(basic_transport* t)
{
	m_connect_retried_count = 0;

	pthread_scoped_lock lk(m_binds_mutex);

	bool ret = m_binds.empty() ? true : false;
	m_binds.push_back(t);

	return ret;
}

bool session::bind_transport(basic_transport* t)
{
	bool ret = basic_session::bind_transport(t);

	pending_queue_t pendings;
	{
		pthread_scoped_lock lk(m_callbacks_mutex);
		pendings.swap(m_pending_queue);
	}

	for(pending_queue_t::iterator it(pendings.begin()),
			it_end(pendings.end()); it != it_end; ++it) {
		t->send_datav(*it, NULL, NULL);
	}

	return ret;
}


bool basic_session::unbind_transport(basic_transport* t, basic_shared_session& self)
{
	pthread_scoped_lock lk(m_binds_mutex);

	binds_t::iterator remove_from =
		std::remove(m_binds.begin(), m_binds.end(), t);
	m_binds.erase(remove_from, m_binds.end());

	if(m_binds.empty()) {
		if(m_manager) {
			wavy::submit(&session_manager::transport_lost_notify, m_manager, self);
		}
		return true;
	}
	return false;
}

bool session::unbind_transport(basic_transport* t, basic_shared_session& self)
{
	return basic_session::unbind_transport(t, self);
}


void basic_session::force_lost(msgobj res, msgobj err)
{
	m_lost = true;
	basic_shared_session nulls;

	pthread_scoped_lock lk(m_callbacks_mutex);
	for(callbacks_t::iterator it(m_callbacks.begin()), it_end(m_callbacks.end());
			it != it_end; ++it) {
		it->second.callback_submit(nulls, res, err);
	}
	m_callbacks.clear();
}


basic_session::callback_entry::callback_entry() { }

basic_session::callback_entry::callback_entry(
		callback_t callback, shared_zone life,
		unsigned short timeout_steps) :
	m_timeout_steps(timeout_steps),
	m_callback(callback),
	m_life(life) { }

void basic_session::callback_entry::callback(basic_shared_session& s,
		msgobj res, msgobj err, auto_zone& z)
{
	m_life->push_finalizer(&mp::object_delete<msgpack::zone>, z.get());
	z.release();
	callback(s, res, err);
}

void basic_session::callback_entry::callback(basic_shared_session& s,
		msgobj res, msgobj err)
try {
	m_callback(s, res, err, m_life);
} catch (std::exception& e) {
	LOG_ERROR("response callback error: ",e.what());
} catch (...) {
	LOG_ERROR("response callback error: unknown error");
}

void basic_session::callback_entry::callback_submit(
		basic_shared_session& s, msgobj res, msgobj err)
{
	wavy::submit(m_callback, s, res, err, m_life);
}

void basic_session::step_timeout(basic_shared_session self)
{
	msgpack::object res;
	res.type = msgpack::type::NIL;
	msgpack::object err;
	err.type = msgpack::type::POSITIVE_INTEGER;
	err.via.u64 = protocol::TIMEOUT_ERROR;

	pthread_scoped_lock lk(m_callbacks_mutex);
	for(callbacks_t::iterator it(m_callbacks.begin()), it_end(m_callbacks.end());
			it != it_end; ) {
		if(!it->second.step_timeout()) {
			LOG_DEBUG("callback timeout this=",(void*)this," id=",it->first);
			it->second.callback(self, res, err);  // client::step_timeout;
			m_callbacks.erase(it++);
		} else {
			++it;
		}
	}
}

bool basic_session::callback_entry::step_timeout()
{
	if(m_timeout_steps > 0) {
		--m_timeout_steps;  // FIXME atomic?
		return true;
	}
	return false;
}


}  // namespace rpc

