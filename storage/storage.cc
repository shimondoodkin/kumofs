#include "storage/storage.h"
#include "log/mlogger.h"

namespace kumo {


inline uint64_t Storage::ihash_of(const char* raw_key)
{
	return *(uint64_t*)raw_key;
}

inline Storage::slot& Storage::slot_of(const char* raw_key, uint32_t raw_keylen)
{
	if(raw_keylen < KEY_META_SIZE) {
		throw std::runtime_error("invalid key");
	}
	return m_slots[((uint32_t)ihash_of(raw_key)) % m_slots_size];
}

inline Storage::entry& Storage::slot::entry_of(const char* raw_key)
{
	return m_entries[((uint32_t)(ihash_of(raw_key) >> 32)) % m_entries_size];
}


inline int Storage::const_db::get(const char* key, uint32_t keylen,
		char* valbuf, uint32_t vallen)
{
	return tchdbget3(m_db, key, keylen, valbuf, vallen);
}

inline bool Storage::const_db::get_clocktime(const char* key, uint32_t keylen, ClockTime* result)
{
	char buf[8];
	int len = get(key, keylen, buf, sizeof(buf));
	if(len < (int32_t)sizeof(buf)) { return false; }
	*result = ClockTime( kumo_be64(*(uint64_t*)buf) );
	return true;
}

inline int Storage::const_db::vsiz(const char* key, uint32_t keylen)
{
	return tchdbvsiz(m_db, key, keylen);
}


Storage::Storage(const char* path)
{
	m_db = tchdbnew();
	if(!m_db) {
		throw std::runtime_error("can't create tchdb");
	}

	//if(!tchdbsetmutex(m_db)) {
	//	throw std::runtime_error("can't initialize tchdb mutex");
	//}

	if(!tchdbopen(m_db, path, HDBOWRITER|HDBOCREAT)) {
		tchdbdel(m_db);
		throw std::runtime_error("can't open tchdb");
	}
}

Storage::~Storage()
{
	tchdbclose(m_db);
	tchdbdel(m_db);
}


const char* Storage::get(const char* raw_key, uint32_t raw_keylen,
		uint32_t* result_raw_vallen, msgpack::zone& z)
{
	while(true) {
		{
			mp::pthread_scoped_rdlock lk(m_global_lock);
			const char* val;
			if( slot_of(raw_key, raw_keylen).get(
						raw_key, raw_keylen,
						const_db(m_db),
						&val, result_raw_vallen, z) ) {
				return val;
			}
		}
		{
			mp::pthread_scoped_wrlock lk(m_global_lock);
			flush();
		}
	}
}

bool Storage::update(const char* raw_key, uint32_t raw_keylen,
		const char* raw_val, uint32_t raw_vallen)
{
	if(raw_vallen < VALUE_META_SIZE) {
		throw std::runtime_error("bad data");
	}

	while(true) {
		{
			mp::pthread_scoped_rdlock lk(m_global_lock);
			bool updated;
			if( slot_of(raw_key, raw_keylen).update(
						raw_key, raw_keylen,
						raw_val, raw_vallen,
						const_db(m_db),
						&updated) ) {
				if(updated) {
					dirty_exist = true;
				}
				return updated;
			}
		}
		{
			mp::pthread_scoped_wrlock lk(m_global_lock);
			flush();
		}
	}
}

bool Storage::del(const char* raw_key, uint32_t raw_keylen,
		ClockTime ct, ClockTime* result_clocktime)
{
	while(true) {
		{
			mp::pthread_scoped_rdlock lk(m_global_lock);
			bool deleted;
			if( slot_of(raw_key, raw_keylen).del(
						raw_key, raw_keylen,
						ct,
						const_db(m_db),
						&deleted, result_clocktime) ) {
				if(deleted) {
					dirty_exist = true;
				}
				return deleted;
			}
		}
		{
			mp::pthread_scoped_wrlock lk(m_global_lock);
			flush();
		}
	}
}


bool Storage::slot::get(const char* raw_key, uint32_t raw_keylen,
		const_db cdb,
		const char** result_raw_val, uint32_t* result_raw_vallen,
		msgpack::zone& z)
{
	mp::pthread_scoped_lock lk(m_mutex);

	entry& e( entry_of(raw_key) );

	if(e.key_equals(raw_key, raw_keylen)) {
		if(e.dirty == DIRTY_DELETE) {
			*result_raw_val = NULL;
			*result_raw_vallen = 0;
			return true;
		}

	} else {

		if(e.dirty != CLEAN) { return false; }

		if( !get_entry(e, cdb, raw_key, raw_keylen) ) {
			// not found
			*result_raw_val = NULL;
			*result_raw_vallen = 0;
			return true;
		}
	}

	z.allocate<mp::shared_buffer::reference>(e.ref);
	*result_raw_val = e.ptr + e.keylen;
	*result_raw_vallen = e.buflen - e.keylen;

	return true;
}


bool Storage::slot::update(const char* raw_key, uint32_t raw_keylen,
		const char* raw_val, uint32_t raw_vallen,
		const_db cdb,
		bool* result_updated)
{
	mp::pthread_scoped_lock lk(m_mutex);

	entry& e( entry_of(raw_key) );

	if(e.key_equals(raw_key, raw_keylen)) {
		if(clocktime_of(raw_val) < e.clocktime()) {
			*result_updated = false;
			return true;
		}

	} else {

		ClockTime ct(0);
		if(cdb.get_clocktime(raw_key, raw_keylen, &ct)) {
			if(clocktime_of(raw_val) < ct) {
				*result_updated = false;
				return true;
			}
		}

		if(e.dirty != CLEAN) { return false; }

		if(e.buflen < raw_keylen + raw_vallen) {
			// FIXME init size
			e.ptr = (char*)m_buffer.allocate(raw_keylen + raw_vallen, &e.ref);
		}

		e.keylen = raw_keylen;
		memcpy(e.ptr, raw_key, raw_keylen);
	}

	e.buflen = e.keylen + raw_vallen;
	memcpy(e.ptr + e.keylen, raw_val, raw_vallen);

	e.dirty = DIRTY_SET;
	*result_updated = true;

	return true;
}

bool Storage::slot::del(const char* raw_key, uint32_t raw_keylen,
		ClockTime ct,
		const_db cdb,
		bool* result_deleted, ClockTime* result_clocktime)
{
	mp::pthread_scoped_lock lk(m_mutex);

	entry& e( entry_of(raw_key) );

	if(e.key_equals(raw_key, raw_keylen)) {
		if(e.dirty == DIRTY_DELETE) {
			*result_deleted = false;
			return true;
		}

	} else {

		if(e.dirty != CLEAN) { return false; }

		if( !get_entry(e, cdb, raw_key, raw_keylen) ) {
			// not found
			*result_deleted = false;
			return true;
		}
	}

	*result_clocktime = e.clocktime();
	if(ct < e.clocktime()) {
		// FIXME return true?
		*result_deleted = false;
		return true;
	}

	e.dirty = DIRTY_DELETE;
	*result_deleted = true;

	return true;
}

bool Storage::slot::get_entry(entry& e, const_db cdb, const char* raw_key, uint32_t raw_keylen)
{
	m_buffer.reserve(1024);  // FIXME init size

	char* buf;
	int sz;

	retry:
	{
		buf = (char*)m_buffer.buffer();

		sz = cdb.get(raw_key, raw_keylen,
				buf, m_buffer.buffer_capacity() - raw_keylen);

		if(sz < (int)VALUE_META_SIZE) {
			// not found
			return false;
		}

		if((size_t)sz == m_buffer.buffer_capacity() - raw_keylen) {
			// insufficient buffer
			sz = cdb.vsiz(raw_key, raw_keylen);
			if(sz < 0) {
				// not found
				return false;
			}
			m_buffer.reserve(sz);
			goto retry;
		}
	}

	e.ptr = buf;
	e.keylen = raw_keylen;
	e.buflen = raw_keylen + sz;

	memcpy(e.ptr, raw_key, raw_keylen);

	m_buffer.allocate(e.buflen, &e.ref);

	return true;
}
	

void Storage::try_flush()
{
	if(!dirty_exist) { return; }
	mp::pthread_scoped_wrlock lk(m_global_lock);
	flush();
}

void Storage::flush()
{
	for(slot* s=m_slots, * const send(m_slots+m_slots_size);
			s != send; ++s) {
		s->flush(m_db);
	}
	dirty_exist = false;
}

inline void Storage::slot::flush(TCHDB* db)
{
	for(entry* e=m_entries, * const eend(m_entries+m_entries_size);
			e != eend; ++e) {
		switch(e->dirty) {
		case CLEAN:
			// do nothing
			break;

		case DIRTY_SET:
			// FIXME error handling
			tchdbput(db, e->ptr, e->keylen, e->ptr+e->keylen, e->buflen-e->keylen);
			e->dirty = CLEAN;
			break;

		case DIRTY_DELETE:
			// FIXME error handling
			tchdbout(db, e->ptr, e->keylen);
			e->dirty = CLEAN;
			break;
		}
	}
}


bool Storage::iterator::del(ClockTime ct)
{
	ClockTime entry_time(0);
	if( !const_db(m_db).get_clocktime(key(), keylen(), &entry_time) ) {
		return false;
	}
	if(ct < entry_time) {
		return false;
	}
	return del_nocheck();
}

bool Storage::iterator::del_nocheck()
{
	return tchdbout(m_db, key(), keylen());
	// FIXME slotを無効化する
}


void Storage::copy(const char* dstpath)
{
	mp::pthread_scoped_wrlock lk(m_global_lock);

	flush();

	if(!tchdbcopy(m_db, dstpath)) {
		LOG_ERROR("DB copy error: ",tchdberrmsg(tchdbecode(m_db)));
		throw std::runtime_error("copy failed");
	}
}

uint64_t Storage::rnum()
{
	mp::pthread_scoped_rdlock lk(m_global_lock);
	return tchdbrnum(m_db);
}

std::string Storage::error()
{
	return std::string(tchdberrmsg(tchdbecode(m_db)));
}


}  // namespace kumo
