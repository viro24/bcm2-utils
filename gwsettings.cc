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

#include <boost/crc.hpp>
#include <openssl/aes.h>
#include <openssl/md5.h>
#include <algorithm>
#include "gwsettings.h"
#include "nonvol.h"
using namespace std;
using namespace bcm2dump;

namespace bcm2cfg {
namespace {
string read_stream(istream& is)
{
	string ret;
	char buf[1024];
	while (is.good()) {
		auto bytes = is.readsome(buf, sizeof(buf));
		if (!bytes) {
			break;
		}

		ret += string(buf, bytes);
	}

	return ret;
}

string group_header_to_string(const string& type, const string& checksum, bool is_chksum_valid, size_t size, bool is_size_valid,
		const string& key, bool is_encrypted, const string& profile, bool is_auto_profile)
{
	ostringstream ostr;
	ostr << "type    : " << type << endl;
	ostr << "profile : ";
	if (profile.empty()) {
		ostr << " (unknown)" << endl;
	} else {
		ostr << profile << (is_auto_profile ? "" : " (forced)") << endl;
	}
	ostr << "checksum: " << checksum << (is_chksum_valid ? "" : " (bad)") << endl;
	ostr << "size    : " << size << (is_size_valid ? "" : " (bad)") << endl;
	if (is_encrypted) {
		ostr << "key     :" << (key.empty() ? "(unknown)" : to_hex(key)) << endl;
	}

	return ostr.str();
}

class permdyn : public settings
{
	public:
	permdyn(bool dyn, const csp<bcm2dump::profile>& p)
	: settings("permdyn", dyn ? nv_group::type_dyn : nv_group::type_perm, p) {}

	virtual size_t bytes() const override
	{ return m_size.num(); }

	virtual size_t data_bytes() const override
	{ return bytes() - 8; }

	virtual istream& read(istream& is) override
	{
		// actually, there's 16 more bytes at the beginning, but these have already been read
		// by gwsettings::read_file, and determined to be all \xff
		string magic(0xba, '\0');
		if (!is.read(&magic[0], magic.size()) || !m_size.read(is) || !m_checksum.read(is)) {
			throw runtime_error("failed to read header");
		}

		if (magic.find_first_not_of('\xff') != string::npos) {
			throw runtime_error("found non-0xff byte in magic");
		}

		string buf = read_stream(is).substr(0, m_size.num() + 16);
		uint32_t crc = crc32(buf);

		if (crc == m_checksum.num()) {
			logger::e() << "checksum ok: " << to_hex(crc) << endl;
		} else {
			logger::e() << "checksum mismatch: " << to_hex(crc) << " / " << to_hex(m_checksum.num()) << endl;
		}

		istringstream istr(buf);
		settings::read(istr);

		return is;
	}

	virtual ostream& write(ostream& os) const override
	{
		ostringstream ostr;
		settings::write(ostr);
		string buf = ostr.str();

		if (!(os << string(0xca, '\xff'))) {
			throw runtime_error("failed to write magic");
		}

		if (!nv_u32(8 + buf.size()).write(os) || !nv_u32(crc32(buf)).write(os)) {
			throw runtime_error("failed to write header");
		}

		if (!os.write(buf.data(), buf.size())) {
			throw runtime_error("failed to write data");
		}

		return os;
	}

	virtual string header_to_string() const override
	{
		return group_header_to_string("permdyn", to_hex(m_checksum.num()), m_checksum_valid,
				m_size.num(), true, "", false, "", false);
	}

	private:
	static uint32_t crc32(const string& buf)
	{
		boost::crc_32_type crc;
		return for_each(buf.begin(), buf.end(), crc)() ^ 0xffffffff;
	}

	nv_u32 m_size;
	nv_u32 m_checksum;
	bool m_checksum_valid = false;
};

class gwsettings : public settings
{
	public:
	gwsettings(const string& checksum, const csp<bcm2dump::profile>& p)
	: settings("gwsettings", nv_group::type_cfg, p), m_checksum(checksum) {}

	virtual size_t bytes() const override
	{ return m_size.num(); }

	virtual size_t data_bytes() const override
	{ return bytes() - (s_magic.size() + 6); }

	virtual string type() const override
	{ return "gwsettings"; }

	virtual istream& read(istream& is) override
	{
		string buf = read_stream(is);
		string magic = buf.substr(0, s_magic.size());
		validate_checksum_and_detect_profile(buf);
		m_magic_valid = (magic == s_magic);

		if (!m_magic_valid && !decrypt_and_detect_profile(buf)) {
			m_encrypted = true;
			return is;
		}

		istringstream istr(buf.substr(s_magic.size()));
		if (!m_version.read(istr) || !m_size.read(istr)) {
			throw runtime_error("error while reading header");
		}

		m_size_valid = m_size.num() == buf.size();

		if (!m_size_valid) {
			if (m_size.num() + 16 == buf.size()) {
				m_padded = true;
				m_size_valid = true;
			}
		}

		settings::read(istr);
		return is;
	}

	virtual ostream& write(ostream& os) const override
	{
		if (!profile()) {
			throw runtime_error("cannot write file without a profile");
		}

		ostringstream ostr;
		settings::write(ostr);
		string buf = ostr.str();

		ostr.str("");
		ostr.write(s_magic.data(), s_magic.size());
		m_version.write(ostr);
		// 2 bytes for version, 4 for size
		nv_u32(s_magic.size() + 6 + buf.size(), false).write(ostr);

		cout << "ostr.str() = " << to_hex(ostr.str()) << endl;

		buf = ostr.str() + buf;
		if (!m_key.empty()) {
			buf = crypt(buf, m_key, false, m_padded);
		}

		if (!(os << calc_checksum(buf, m_profile))) {
			throw runtime_error("error while writing checksum");
		}

		if (!(os.write(buf.data(), buf.size()))) {
			throw runtime_error("error while writing data");
		}

		if (m_padded && !(os << string(16, '\0'))) {
			throw runtime_error("error while writing padding");
		}

		return os;
	}

	virtual string header_to_string() const override
	{
		return group_header_to_string("gwsettings", to_hex(m_checksum), m_checksum_valid,
				m_size.num(), m_size_valid, m_key, m_encrypted, profile() ? profile()->name() : "",
				m_is_auto_profile);
	}

	private:
	string m_checksum;

	void validate_checksum_and_detect_profile(const string& buf)
	{
		if (profile()) {
			validate_checksum(buf, profile());
		} else {
			for (auto p : profile::list()) {
				if (validate_checksum(buf, p)) {
					m_is_auto_profile = true;
					m_profile = p;
					break;
				}
			}
		}
	}

	bool validate_checksum(const string& buf, const csp<bcm2dump::profile>& p)
	{
		m_checksum_valid = (m_checksum == calc_checksum(buf, p));
		return m_checksum_valid;
	}

	static string calc_checksum(const string& buf, const csp<bcm2dump::profile>& p)
	{
		MD5_CTX c;
		MD5_Init(&c);
		MD5_Update(&c, buf.data(), buf.size());

		string key = p ? p->md5_key() : "";
		if (!key.empty()) {
			MD5_Update(&c, key.data(), key.size());
		}

		string md5(16, '\0');
		MD5_Final(reinterpret_cast<unsigned char*>(&md5[0]), &c);
		return md5;
	}

	bool decrypt_and_detect_profile(string& buf)
	{
		if (!m_key.empty()) {
			return decrypt(buf, m_key);
		} else if (profile()) {
			return decrypt_with_profile(buf, profile());
		} else {
			for (auto p : profile::list()) {
				if (decrypt_with_profile(buf, p)) {
					return true;
				}
			}
		}

		return false;
	}

	bool decrypt_with_profile(string& buf, const csp<bcm2dump::profile>& p)
	{
		for (auto k : p->default_keys()) {
			if (decrypt(buf, k)) {
				m_key = k;
				return true;
			}
		}

		return false;
	}

	bool decrypt(string& buf, const string& key)
	{
		string decrypted = crypt(buf, key, true);
		if (decrypted.substr(0, 74) == s_magic) {
			buf = decrypted;
			m_magic_valid = true;
			return true;
		}

		return false;
	}

	static std::string crypt(string ibuf, const string& key, bool decrypt, bool pad = false)
	{
		auto k = reinterpret_cast<const unsigned char*>(key.data());
		AES_KEY aes;

		if (decrypt) {
			AES_set_decrypt_key(k, 256, &aes);
		} else {
			AES_set_encrypt_key(k, 256, &aes);

			if (pad) {
				ibuf += string(16, '\0');
			}
		}

		string obuf(ibuf.size(), '\0');

		auto remaining = ibuf.size();
		// this is legal in C++11... ugly, but legal!
		auto iblock = reinterpret_cast<unsigned char*>(&ibuf[0]);
		auto oblock = reinterpret_cast<unsigned char*>(&obuf[0]);

		while (remaining >= 16) {
			if (decrypt) {
				AES_decrypt(iblock, oblock, &aes);
			} else {
				AES_encrypt(iblock, oblock, &aes);
			}

			remaining -= 16;
			iblock += 16;
			oblock += 16;
		}

		if (remaining) {
			memcpy(oblock, iblock, remaining);
		}

		return obuf;
	}

	bool m_is_auto_profile = false;
	bool m_checksum_valid = false;
	bool m_magic_valid = false;
	bool m_size_valid = false;
	bool m_encrypted = false;
	nv_version m_version;
	nv_u32 m_size;
	string m_key;
	bool m_padded = false;

	static const string s_magic;
};
const string gwsettings::s_magic = "6u9E9eWF0bt9Y8Rw690Le4669JYe4d-056T9p4ijm4EA6u9ee659jn9E-54e4j6rPj069K-670";
}

istream& settings::read(istream& is)
{
	sp<nv_group> group;
	size_t remaining = data_bytes();
	unsigned mult = 1;

	while (remaining && !is.eof()) {
		if (!nv_group::read(is, group, m_type, remaining) && !is.eof()) {
			if (!m_permissive) {
				throw runtime_error("failed to read group " + group->magic().to_str());
			}
			// fake eof
			is.clear(ios::eofbit);
			break;
		} else {
			string name = group->name();
			if (find(name)) {
				name += "_" + std::to_string(++mult);
				logger::v() << "redefinition of " << group->name() << " renamed to " << name << endl;
			}
			m_groups.push_back( { name, group });
			remaining -= group->bytes();
		}
	}

	return is;
}


sp<settings> settings::read(istream& is, int type, const csp<bcm2dump::profile>& p, const std::string& key)
{
	string start(16, '\0');
	if (!is.read(&start[0], start.size())) {
		throw runtime_error("failed to read file");
	}

	sp<settings> ret;
	if (start == string(16, '\xff')) {
		if (type == nv_group::type_dyn || type == nv_group::type_perm) {
			ret = sp<permdyn>(new permdyn(type == nv_group::type_dyn, p));
		} else {
			logger::w() << "file looks like a permnv/dynnv file, but no type was specified" << endl;
		}
	}

	if (!ret) {
		// if this is in fact a gwsettings type file, then start already contains the checksum
		//ret = make_shared<gwsettings>(start, p);
		ret = sp<gwsettings>(new gwsettings(start, p));
	}

	if (ret) {
		ret->read(is);
	}

	return ret;
}





}
