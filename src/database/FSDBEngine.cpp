#include "IDatabaseEngine.h"
#include "DBEngineFactory.h"
#include "util/Datagram.h"
#include "util/DatagramIterator.h"
#include "core/logger.h"
#include "core/global.h"
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <list>

ConfigVariable<std::string> foldername("foldername", "objs");
LogCategory fsdb_log("fsdb", "Filesystem Database Engine");

class FSDBEngine : public IDatabaseEngine
{
private:
	unsigned int m_next_id;
	std::list<unsigned int> m_free_ids;
	std::string m_foldername;

	// update_next_id updates "id.txt" on the disk with the next available id
	void update_next_id()
	{
		std::fstream file;
		file.open(m_foldername + "/id.txt", std::ios_base::out);
		if(file.is_open())
		{
			file << m_next_id;
			file.close();
		}
	}

	// update_free_ids updates "free.dat" on the disk with the current list of freed ids
	void update_free_ids()
	{
		std::fstream file;
		file.open(m_foldername + "/free.dat", std::ios_base::out | std::ios_base::binary);
		if(file.is_open())
		{
			Datagram dg;
			dg.add_uint32(m_free_ids.size());
			for(auto it = m_free_ids.begin(); it != m_free_ids.end(); ++it)
			{
				dg.add_uint32(*it);
			}
			file.write(dg.get_data(), dg.get_buf_end());
			file.close();
		}
	}

	// get_next_id returns the next available id to be used in object creation
	unsigned int get_next_id()
	{
		unsigned int do_id;
		if(m_next_id <= m_max_id)
		{
			do_id = m_next_id++;
			update_next_id();
		}
		else
		{
			// Dequeue id from list
			if(!m_free_ids.empty())
			{
				do_id = *m_free_ids.begin();
				m_free_ids.remove(do_id);
				update_free_ids();
			}
			else
			{
				return 0;
			}
		}
		return do_id;
	}
public:
	FSDBEngine(DBEngineConfig dbeconfig, unsigned int min_id, unsigned int max_id) :
		IDatabaseEngine(dbeconfig, min_id, max_id),
		m_next_id(min_id), m_free_ids(),
		m_foldername(foldername.get_rval(m_config))
	{
		std::fstream file;

		// Get next id from "id.txt" in database
		file.open(m_foldername + "/id.txt", std::ios_base::in);
		if(file.is_open())
		{
			file >> m_next_id;
			file.close();
		}

		// Get list of free ids from "free.dat" in database
		file.open(m_foldername + "/free.dat", std::ios_base::in | std::ios_base::binary);
		if(file.is_open())
		{
			file.seekg(0, std::ios_base::end);
			unsigned int len = file.tellg();
			char *data = new char[len];
			file.seekg(0, std::ios_base::beg);
			file.read(data, len);
			file.close();
			Datagram dg(std::string(data, len));
			delete [] data; //dg makes a copy
			DatagramIterator dgi(dg);

			unsigned int num_ids = dgi.read_uint32();
			for(unsigned int i = 0; i < num_ids; ++i)
			{
				auto k = dgi.read_uint32();
				fsdb_log.spam() << "Loaded free id: " << k << std::endl;
				m_free_ids.insert(m_free_ids.end(), k);
			}
		}
	}

	virtual unsigned int create_object(const DatabaseObject &dbo)
	{
		unsigned int do_id = get_next_id();
		if(do_id == 0)
		{
			return 0;
		}

		// Prepare object filename
		std::stringstream filename;
		filename << m_foldername << "/" << do_id << ".dat";

		// Write object to file
		std::fstream file;
		file.open(filename.str(), std::ios_base::out | std::ios_base::binary);
		if(file.is_open())
		{
			Datagram dg;
			dg.add_uint16(dbo.dc_id);
			dg.add_uint16(dbo.fields.size());
			for(auto it = dbo.fields.begin(); it != dbo.fields.end(); ++it)
			{
				dg.add_uint16(it->first->get_number());
				dg.add_string(it->second);
			}
			file.write(dg.get_data(), dg.get_buf_end());
			file.close();
			return do_id;
		}
		
		return 0;
	}

	virtual bool get_object(unsigned int do_id, DatabaseObject &dbo)
	{
		std::stringstream filename;
		filename << m_foldername << "/" << do_id << ".dat";

		std::fstream file;
		file.open(filename.str(), std::ios_base::in | std::ios_base::binary);
		if(file.is_open())
		{
			try
			{
				file.seekg(0, std::ios_base::end);
				unsigned int len = file.tellg();
				char *data = new char[len];
				file.seekg(0, std::ios_base::beg);
				file.read(data, len);
				file.close();
				Datagram dg(std::string(data, len));
				delete [] data; //dg makes a copy
				DatagramIterator dgi(dg);

				dbo.dc_id = dgi.read_uint16();
				DCClass *dcc = gDCF->get_class(dbo.dc_id);
				if(!dcc)
				{
					std::stringstream ss;
					ss << "DCClass " << dbo.dc_id << "does not exist.";
					throw std::runtime_error(ss.str());
				}

				unsigned short field_count = dgi.read_uint16();
				for(unsigned int i = 0; i < field_count; ++i)
				{
					unsigned short field_id = dgi.read_uint16();
					DCField *field = dcc->get_field_by_index(field_id);
					if(!field)
					{
						std::stringstream ss;
						ss << "DCField " << field_id << " does not exist in DCClass " << dbo.dc_id;
						throw std::runtime_error(ss.str());
					}
					dbo.fields[field] = dgi.read_string();
				}
				return true;
			}
			catch (std::exception &e)
			{
				fsdb_log.error() << "Exception in get_object while trying to read do_id: #"
					<< do_id << " e.what(): " << e.what() << std::endl;
			}
		}

		return false;
	}

	virtual void delete_object(unsigned int do_id)
	{
		std::stringstream filename;
		filename << foldername.get_rval(m_config) << "/" << do_id << ".dat";
		fsdb_log.debug() << "Deleting file: " << filename.str() << std::endl;
		if(!std::remove(filename.str().c_str()))
		{
			m_free_ids.insert(m_free_ids.end(), do_id);
			update_free_ids();
		}
	}
};

DBEngineCreator<FSDBEngine> fsdbengine_creator("filesystem");