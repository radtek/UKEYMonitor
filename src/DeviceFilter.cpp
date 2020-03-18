#include "DeviceFilter.h"
#include "Poco/Debugger.h"
#include "Poco/String.h"
#include "Poco/RegularExpression.h"
#include "Poco/File.h"
#include "Poco/FileStream.h"
#include "Poco/StreamCopier.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Util/Application.h"
#include "Poco/Data/SessionFactory.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/RecordSet.h"
#include "Poco/Data/Column.h"
#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Util/WinService.h"
#include "Poco/Thread.h"
#include "Poco/NamedEvent.h"
#include <sstream>
#include <cassert>


using namespace Reach;
using Poco::Debugger;
using Poco::format;
using Poco::isubstr;
using Poco::istring;
using Poco::replace;
using Poco::Path;
using Poco::File;
using Poco::FileInputStream;
using Poco::StreamCopier;
using Poco::JSON::Parser;
using Poco::RegularExpression;
using Poco::Util::Application;
using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Data::RecordSet;
using Poco::Util::WinService;
using Poco::Thread;
using Poco::NamedEvent;
using namespace Poco::Data::Keywords;

static NamedEvent DeviceChangedEvent("DeviceChangedEvent");

DeviceFilter::DeviceFilter(const std::string& enumerate_id, bool presented)
	:_enumerate(enumerate_id), _presented(presented)
{
	try
	{
		loadConfigure();
		enqueue();
		resetRESTfulService();
	}
	catch (Poco::Exception& e)
	{
		dbgview(format("DeviceFilter Exception : %d - %s - %s - %s", e.code(), e.displayText(), std::string(e.what()), std::string(e.className())));
	}

}

DeviceFilter::~DeviceFilter()
{

}

std::string DeviceFilter::current()
{
	Application& app = Application::instance();
	Path appPath(app.commandPath());
	std::string fullname = appPath.getFileName();
	return Poco::replace(app.commandPath(), fullname, std::string(""));
}

void DeviceFilter::loadConfigure()
{
	Application& app = Application::instance();
	std::string configuration = app.config().getString("application.devices.configfile");

	Path filePath(current(), configuration);
	dbgview(format("presented :%b configuration filePath : %s", _presented, filePath.toString()));

	std::ostringstream ostr;
	if (filePath.isFile())
	{
		File inputFile(filePath);
		if (inputFile.exists())
		{
			FileInputStream fis(filePath.toString());
			StreamCopier::copyStream(fis, ostr);
		}
	}
	std::string jsonStr = ostr.str();
	Parser sparser;
	Var result = sparser.parse(jsonStr);
	Poco::JSON::Array::Ptr arr = result.extract<Poco::JSON::Array::Ptr>();
	_data = *arr;
}

void DeviceFilter::enqueue()
{
	std::string pattern("\\?\\\\(\\S+)#(\\S+)#(\\S+)#(\\S+)");
	int options = 0;
	try
	{
		RegularExpression re(pattern, options);
		RegularExpression::Match mtch;

		if (!re.match(_enumerate, mtch))
			return;

		std::vector<std::string> tags;
		re.split(_enumerate, tags, options);
		/// lpdbv->dbcc_name tags 0 : ?\USB#VID_1D99&PID_0001#5&38e97a59&0&10#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
		/// lpdbv->dbcc_name tags 1 : USB					# enumerator
		///	lpdbv->dbcc_name tags 2 : VID_1D99&PID_0001   # hardware identification string
		///	lpdbv->dbcc_name tags 3 : 5&38e97a59&0&10	  # os-specific-instance
		///	lpdbv->dbcc_name tags 4 : {a5dcbf10-6530-11d2-901f-00c04fb951ed} # class guid
		/// Sqlite table : "CREATE TABLE DeviceSet (Description VARCHAR(30), ENUMERATOR VARCHAR(32), HardwareID VARCHAR(200), InstanceID VARCHAR(32), ClassGUID VARCHAR(39)), PRESENT BOOLEAN(1)"
		if (isLegelDevice(tags[2])) {

			DeviceInfoType info(_description, tags[1], tags[2], tags[3], tags[4], _engine);
			updateDeviceStatus(info, _presented);

			DeviceChangedEvent.set();
			dbgview("UKEYMonitor DeviceChangedEvent set");
			dbgview(format("Update SQLite ->\n Description : %s\n  enumerator : %s\n HardwareID : %s\n InstanceID : %s\n   ClassGUID : %s\n",
				_description, tags[1], tags[2], tags[3], tags[4]));

			_lastChangedDevice = tags[2];
		}
	}
	catch (Poco::RegularExpressionException& e)
	{
		dbgview(format("lpdbv->dbcc_name tags %d : %s", e.code(), e.displayText()));
	}
}

void DeviceFilter::updateDeviceStatus(const DeviceInfoType& info, bool present)
{
	std::string description(info.get<description>());
	std::string enumerator(info.get<enumerator>());
	std::string hardware(info.get<hardware_id>());
	std::string instance(info.get<instance_id>());
	std::string classGuid(info.get<class_guid>());
	std::string engine(info.get<engine_mode>());

	std::vector<std::string> HardwareSet;

	Session session("SQLite", "DeQLite.db");

	if (!present)
	{
		session << "SELECT * FROM DeviceSet WHERE HardwareID = ? AND InstanceID = ?",
			use(hardware),
			use(instance),
			into(HardwareSet),
			now;

		if (HardwareSet.empty()) return;

		session << "DELETE FROM DeviceSet WHERE HardwareID = ? AND InstanceID = ?",
			use(hardware), use(instance), now;
	}
	else session << "INSERT INTO DeviceSet VALUES(?, ?, ?, ?, ?, ?,?)",
			use(description), //_description
			use(enumerator), //tags[1] 
			use(hardware), //tags[2]
			use(instance), //tags[3]
			use(classGuid), //tags[4]
			use(engine),	// engine_mode
			use(present),  // _presented
			now;
}

void DeviceFilter::resetRESTfulService()
{
	if (!_presented && _lastChangedDevice == "VID_5448&PID_0004")
	{
		/// specific when swap multiple device by user ,ukey cannot not work.
		Poco::Timestamp ts;
		dbgview(Poco::DateTimeFormatter::format(ts, "%dd %H:%M:%S.%i"));
#ifdef _DEBUG
		stopService("rsyncClientd");
		restartService("rsyncAgentd");
		startService("rsyncClientd");
#else
		stopService("rsyncClient");
		restartService("rsyncAgent");
		startService("rsyncClient");
#endif // _DEBUG	
		ts.update();
		dbgview(Poco::DateTimeFormatter::format(ts, "%dd %H:%M:%S.%i"));
	}
}

#include "Poco/Stopwatch.h"

void DeviceFilter::startService(const std::string& name)
{
	WinService service(name);
	if (!service.isRegistered() || service.isRunning()) return;

	size_t tout = 500 * 10;
	Poco::Stopwatch sw; sw.start();

	while (!service.isRunning()) {

		try { Poco::Thread::sleep(200); service.start(); }
		catch (Poco::SystemException&) {}

		if (sw.elapsedSeconds() >= tout)
			throw Poco::TimeoutException("startService failed!", service.name());
		else
			Poco::Thread::sleep(10);
	}
}

void DeviceFilter::stopService(const std::string& name)
{
	WinService service(name);
	if (!service.isRegistered() || !service.isRunning()) return;

	size_t tout = 500 * 10;
	Poco::Stopwatch sw; sw.start();

	while (service.isRunning()) {

		try { Poco::Thread::sleep(200); service.stop(); }
		catch (Poco::SystemException&) {}

		if (sw.elapsedSeconds() >= tout)
			throw Poco::TimeoutException("stopService failed!", service.name());
		else
			Poco::Thread::sleep(10);
	}
}

void DeviceFilter::restartService(const std::string& name)
{
	WinService service(name);

	try
	{
		if (service.isRunning()) {
			service.stop();
		}

		size_t tout = 1000 * 10;
		Poco::Stopwatch sw; sw.start();

		while (!service.isRunning()) {

			try { Poco::Thread::sleep(200); service.start(); }
			catch (Poco::SystemException&) {}

			if (sw.elapsedSeconds() >= tout)
			{
				service.start();
				throw Poco::TimeoutException(service.name(), "restartService");
			}
			else Poco::Thread::sleep(10);
		}
	}
	catch (Poco::Exception& e)
	{
		dbgview(e.message());
	}
}

bool DeviceFilter::isLegelDevice(const std::string& deivice_id)
{
	for (int i = 0; i < _data.size(); i++) {
		assert(_data[i].isStruct());
		std::string hardware = _data[i]["hardwareID"];
		if (isubstr(hardware, deivice_id) != istring::npos) {
			_description = _data[i]["description"].convert<std::string>();
			_engine = _data[i]["engine"].convert<std::string>();
			dbgview(format("description : %s , engine: %s", _description, _engine));
			return true;
		}

	}
	return false;
}

void DeviceFilter::dbgview(const std::string& message)
{
#ifndef _DEBUG
	::OutputDebugStringA(message.c_str());
#endif // !_NDEBUG
	Debugger::message(message);
}