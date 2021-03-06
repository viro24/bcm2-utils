/**
 * bcm2-utils
 * Copyright (C) 2016 Joseph Lehner <joseph.c.lehner@gmail.com>
 *
 * bcm2-utils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bcm2-utils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with bcm2-utils.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef BCM2UTILS_UTIL_H
#define BCM2UTILS_UTIL_H
#include <type_traits>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <functional>
#include <stdexcept>
#include <typeinfo>
#include <iostream>
#include <iomanip>
#include <netdb.h>
#include <fstream>
#include <sstream>
#include <memory>
#include <cerrno>
#include <vector>
#include <string>
#include <ios>

namespace bcm2dump {

std::string trim(std::string str);
std::vector<std::string> split(const std::string& str, char delim, bool empties = true, size_t limit = 0);

inline bool contains(const std::string& haystack, const std::string& needle)
{
	return haystack.find(needle) != std::string::npos;
}

inline bool is_bfc_prompt(const std::string& str, const std::string& prompt)
{
	return str.find(prompt + ">") != std::string::npos || str.find(prompt + "/") != std::string::npos;
}

template<class T> T extract(const std::string& data, std::string::size_type offset = 0)
{
	return *reinterpret_cast<const T*>(data.substr(offset, sizeof(data)).c_str());
}

template<class T> void patch(std::string& data, std::string::size_type offset, const T& t)
{
	data.replace(offset, sizeof(T), std::string(reinterpret_cast<const char*>(&t), sizeof(T)));
}

class bad_lexical_cast : public std::invalid_argument
{
	public:
	bad_lexical_cast(const std::string& str) : std::invalid_argument(str) {}
};

template<class T> T lexical_cast(const std::string& str, unsigned base = 10)
{
	static std::istringstream istr;
	istr.clear();
	istr.str(str);
	T t;

	if (!base) {
		if (str.size() > 2 && str.substr(0, 2) == "0x") {
			base = 16;
		} else {
			base = 10;
		}
	}

	if ((istr >> std::setbase(base) >> t)) {
		if (!istr.eof() && base == 10) {
			switch (istr.get()) {
			case 'k':
			case 'K':
				t *= 1024;
				break;
			case 'm':
			case 'M':
				t *= 1024 * 1024;
				break;
			}
		}

		if (istr.eof() || istr.get() == '\0') {
			return t;
		}
	}

	throw bad_lexical_cast("conversion failed: '" + str + "' -> " + std::string(typeid(T).name()));
}

template<class T> std::string to_hex(const T& t, size_t width = sizeof(T) * 2)
{
	std::ostringstream ostr;
	ostr << std::setfill('0') << std::setw(width) << std::hex << t;
	return ostr.str();
}

template<> inline std::string to_hex<char>(const char& c, size_t width)
{
	return to_hex(c & 0xff, width);
}

template<> inline std::string to_hex<unsigned char>(const unsigned char& c, size_t width)
{
	return to_hex(c & 0xff, width);
}

std::string to_hex(const std::string& buffer);

// return the closest nv_number lower than nv_num that matches the requested alignment
template<class T> T align_left(const T& nv_num, size_t alignment)
{
	return nv_num - (nv_num % alignment);
}

// return the closest nv_number higher than nv_num that matches the requested alignment
template<class T> T align_right(const T& nv_num, size_t alignment)
{
	T rem = nv_num % alignment;
	return nv_num + (rem ? alignment - rem : 0);
}

uint16_t crc16_ccitt(const void* buf, size_t size);
inline uint16_t crc16_ccitt(const std::string& buf)
{ return crc16_ccitt(buf.data(), buf.size()); }

inline unsigned elapsed_millis(std::clock_t start, std::clock_t now = std::clock())
{
	return 1000 * (now - start) / CLOCKS_PER_SEC;
}

std::string transform(const std::string& str, std::function<int(int)> f);

template<typename T> struct bswapper
{
	static T ntoh(T n);
	static T hton(T n);
};

#define BCM2UTILS_DEF_BSWAPPER(type, f_ntoh, f_hton) \
	template<> struct bswapper<type> \
	{\
		static type ntoh(const type& n) \
		{ return f_ntoh(n); } \
		\
		static type hton(const type& n) \
		{ return f_hton(n); } \
	}

BCM2UTILS_DEF_BSWAPPER(uint8_t,,);
BCM2UTILS_DEF_BSWAPPER(int8_t,,);
BCM2UTILS_DEF_BSWAPPER(uint16_t, ntohs, htons);
BCM2UTILS_DEF_BSWAPPER(int16_t, ntohs, htons);
BCM2UTILS_DEF_BSWAPPER(uint32_t, ntohl ,htonl);
BCM2UTILS_DEF_BSWAPPER(int32_t, ntohl, htonl);

/*
template<> struct bswapper<uint8_t>
{
	static uint8_t ntoh(const uint8_t& n)
	{ return n; }

	static uint8_t hton(const uint8_t& n)
	{ return n; }
};

template<> struct bswapper<uint16_t>
{
	static uint16_t ntoh(const uint16_t& n)
	{ return ntohs(n); }

	static uint16_t hton(const uint16_t& n)
	{ return htons(n); }
};

template<> struct bswapper<uint32_t>
{
	static uint32_t ntoh(const uint32_t& n)
	{ return ntohl(n); }

	static uint32_t hton(const uint32_t& n)
	{ return htonl(n); }
};

template<> struct bswapper<int32_t>
{
	static int32_t ntoh(const int32_t& n)
	{ return ntohl(n); }

	static int32_t hton(const int32_t& n)
	{ return htonl(n); }
};
*/

template<typename T> using sp = std::shared_ptr<T>;
template<typename T> using csp = std::shared_ptr<const T>;

class scoped_ios_exceptions
{
	public:
	~scoped_ios_exceptions()
	{
		try {
			m_ios.exceptions(m_saved);
		} catch (...) {}
	}

	static scoped_ios_exceptions failbad(std::ios& ios)
	{ return scoped_ios_exceptions(ios, std::ios::failbit | std::ios::badbit, false); }

	static scoped_ios_exceptions none(std::ios& ios)
	{ return scoped_ios_exceptions(ios, std::ios::goodbit, true); }

	private:
	scoped_ios_exceptions(std::ios& ios, std::ios::iostate except, bool replace)
	: m_ios(ios), m_saved(ios.exceptions())
	{
		ios.exceptions(replace ? except : (m_saved | except));
	}

	std::ios& m_ios;
	std::ios::iostate m_saved;
};

class logger
{
	public:
	static constexpr int trace = 0;
	static constexpr int debug = 1;
	static constexpr int verbose = 2;
	static constexpr int info = 3;
	static constexpr int warn = 4;
	static constexpr int err = 5;

	static std::ostream& log(int severity);

	static std::ostream& t()
	{ return log(trace); }

	static std::ostream& v()
	{ return log(verbose); }

	static std::ostream& d()
	{ return log(debug); }

	static std::ostream& i()
	{ return log(info); }

	static std::ostream& w()
	{ return log(warn); }

	static std::ostream& e()
	{ return log(err); }

	static void loglevel(int level)
	{ s_loglevel = level; }

	static int loglevel()
	{ return s_loglevel; }

	private:
	static std::ofstream s_bucket;
	static int s_loglevel;
};

class user_error : public std::runtime_error
{
	public:
	user_error(const std::string& what)
	: std::runtime_error(what)
	{}
};

class errno_error : public std::system_error
{
	public:
	errno_error(const std::string& what, int error = errno)
	: std::system_error(error, std::system_category(), what), m_interrupted(error == EINTR)
	{}

	bool interrupted() const noexcept
	{ return m_interrupted; }

	private:
	bool m_interrupted;
};

class getaddrinfo_category : public std::error_category
{
	virtual const char* name() const noexcept override
	{ return "getaddrinfo_category"; };

	virtual std::string message(int condition) const override;
};
}

template<class T> struct bcm2dump_def_comparison_operators
{
	friend bool operator!=(const T& lhs, const T& rhs)
	{ return !(lhs == rhs); }

	friend bool operator <=(const T& lhs, const T& rhs)
	{ return lhs < rhs || lhs == rhs; }

	friend bool operator>(const T& lhs, const T& rhs)
	{ return !(lhs <= rhs); }

	friend bool operator>=(const T& lhs, const T& rhs)
	{ return !(lhs < rhs); }
};

#endif
