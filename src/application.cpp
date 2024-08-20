#include <signal.h>

#include <veutil/qt/daemontools_service.hpp>
#include <veutil/qt/ve_dbus_connection.hpp>
#include <veutil/qt/ve_qitem.hpp>
#include <veutil/qt/ve_qitem_exported_dbus_services.hpp>

#include "application.hpp"
#include "security_profiles.hpp"
#include "time.hpp"

#define LYNX_BMS_500 0xA3E5
#define LYNX_BMS_500_NG 0xA3E4
#define LYNX_BMS_1000 0xA3E6
#define LYNX_BMS_1000_NG 0xA3E7

static QDir machineRuntimeDir = QDir("/etc/venus");
static QDir venusDir = QDir("/opt/victronenergy");
QVariant Application::mRunningGui;

bool serviceExists(QString const &svc) {
	return QDir("/service/" + svc).exists();
}

static bool templateExists(QString const &name) {
	return QFile("/opt/victronenergy/service-templates/conf/" + name + ".conf").exists();
}

QStringList getFeatureList(QString const &name, bool lines)
{
	QStringList ret;
	QFile file(machineRuntimeDir.filePath(name));

	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return ret;

	QString line;
	while (!file.atEnd()) {
		line = file.readLine();
		if (lines) {
			line = line.trimmed();
			if (!line.isEmpty())
				ret.append(line);
		} else {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
			ret.append(line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts));
#else
			ret.append(line.split(QRegExp("\\s+"), QString::SkipEmptyParts));
#endif
		}
	}

	return ret;
}

QString getFeature(QString const &name, bool optional)
{
	QStringList list = getFeatureList(name);

	if (!optional && list.count() != 1) {
		qCritical() << "required machine feature " + name + " does not exist";
		exit(EXIT_FAILURE);
	}

	return (list.count() >= 1 ? list[0] : QString());
}

int readIntFromFile(QString const &name, int def)
{
	QFile file(name);
	bool ok;

	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return def;

	QString line = file.readLine();
	int val = line.trimmed().toInt(&ok, 0);
	if (!ok)
		return def;

	return val;
}

bool writeIntToFile(QString filename, int value)
{
	QFile file(filename);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return false;

	QTextStream out(&file);
	out << QString::number(value);
	file.close();

	return true;
}

static bool dataPartionError()
{
	QFile file("/run/data-partition-state");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.atEnd())
		return false;

	QTextStream in(&file);
	QString line = in.readLine();

	return line == "failed" || line == "failed-to-mount";
}

int VeQItemReboot::setValue(const QVariant &value)
{
	qDebug() << "[VeQItemReboot] Queuing a reboot";

	QTimer *timer = new QTimer(this);
	timer->setSingleShot(true);
	connect(timer, SIGNAL(timeout()), this, SLOT(doReboot()));
	connect(timer, SIGNAL(timeout()), timer, SLOT(deleteLater())); // a bit silly, rebooting anyway..
	timer->start(2000);

	return VeQItemAction::setValue(value);
}

void VeQItemReboot::doReboot()
{
	qDebug() << "[VeQItemReboot] Rebooting";

	/*
	 * Note: this used to spawn a child process to call reboot. Since it was found,
	 * that that can fail, just signal init directly. It should have been fixed in
	 * a later linux release...
	 *
	 * @note this only works if init accepts SIGINT.
	 */
	kill(1, SIGINT);
}

enum Mk3Update {
	DISALLOWED,
	ALLOWED,
	NOT_APPLICABLE
};

class SettingsInfo : public VeQItemSettingsInfo
{
public:
	SettingsInfo(enum Mk3Update mk3update)
	{
		QString mBacklightDevice = getFeature("backlight_device");

		add("Gps/Format", 0, 0, 0);
		add("Gps/SpeedUnit", "km/h");

		// Create dbus settings. Do not create settings when capabilities do not exist.
		if (!getFeature("backlight_device").isEmpty()) {
			int mMaxBrightness = readIntFromFile(mBacklightDevice + "/max_brightness", -1);
			add("Gui/Brightness", mMaxBrightness, 1, mMaxBrightness);

			// FIXME: delete the setting in the else, it was unconditionally added in the past,
			// so when switching the gui to use the presence of the setting, it will be incorrect.
			// Hence set the max value to zero for now, if there is no auto brightness.
			bool hasAutoBrightness = readIntFromFile(mBacklightDevice + "/auto_brightness", -1) != -1;
			if (hasAutoBrightness)
				add("Gui/AutoBrightness", 1, 0, 1);
			else
				add("Gui/AutoBrightness", 0, 0, 0);
		}

		add("Gui/DemoMode", 0, 0, 3);
		add("Gui/DisplayOff", 600, 0, 0);
		add("Gui/Language", "en");
		add("Gui/RunningVersion", 1, 1, 2);
		add("Gui/TouchEnabled", 1, 0, 1);
		if (LedController::hasLeds())
			add("LEDs/Enable", 1, 0, 1);
		add("Relay/Function", 0, 0, 0);
		add("Relay/Polarity", 0, 0, 0);
		add("Relay/1/Function", 2, 0, 0);
		add("Relay/1/Polarity", 0, 0, 0);
		if (templateExists("hostapd"))
			add("Services/AccessPoint", 1, 0, 1);
		add("Services/BleSensors", 0, 0, 1);
		add("Services/Bluetooth", 1, 0, 1);
		add("Services/Evcc", 1, 0, 1);
		add("Services/Modbus", 0, 0, 1);
		add("Services/MqttLocal", 0, 0, 1);
		add("Services/NodeRed", 0, 0, 2);
		add("Services/SignalK", 0, 0, 1);
		// Note: only for debugging over tcp/ip, _not_ socketcan itself...
		add("Services/Socketcand", 0, 0, 1);
		add("Services/Tailscale/Enabled", 0, 0, 1);
		add("System/AccessLevel", 1, 0, 3);
		add("System/AutoUpdate", 2, 0, 3);
		add("System/ImageType", 0, 0, 1);
		add("System/LogLevel", 2, 0, 0);
		add("System/ReleaseType", 0, 0, 3);
		add("System/TimeZone", "/UTC");
		add("System/Units/Temperature", "");
		add("System/VolumeUnit", 0, 0, 0);
		add("SystemSetup/SystemName", "");
		add("Vebus/AllowMk3Fw212Update", mk3update, 0, 2);
	}
};

Application::Application::Application(int &argc, char **argv) :
	QCoreApplication(argc, argv),
	mLocalSettingsTimeout()
{
	QDBusConnection dbus = VeDbusConnection::getConnection();
	if (!dbus.isConnected()) {
		qCritical() << "DBus connection failed";
		::exit(EXIT_FAILURE);
	}

	VeQItemDbusProducer *producer = new VeQItemDbusProducer(VeQItems::getRoot(), "dbus");
	producer->open(dbus);
	mServices = producer->services();
	mSettings = new VeQItemDbusSettings(producer->services(), QString("com.victronenergy.settings"));

	VeQItem *item = mServices->itemGetOrCreate("com.victronenergy.settings");
	connect(item, SIGNAL(stateChanged(VeQItem::State)), SLOT(onLocalSettingsStateChanged(VeQItem::State)));
	if (item->getState() == VeQItem::Synchronized) {
		qDebug() << "Localsettings found";
		init();
	} else {
		qDebug() << "Localsettings not found";
		mLocalSettingsTimeout.setSingleShot(true);
		mLocalSettingsTimeout.setInterval(120000);
		connect(&mLocalSettingsTimeout, SIGNAL(timeout()), SLOT(onLocalSettingsTimeout()));
		mLocalSettingsTimeout.start();
	}
}

void Application::onLocalSettingsStateChanged(VeQItem::State state)
{
	mLocalSettingsTimeout.stop();

	if (state != VeQItem::Synchronized) {
		qCritical() << "Localsettings not available" << state;
		::exit(EXIT_FAILURE);
	}

	qDebug() << "Localsettings appeared";
	init();
}

void Application::onLocalSettingsTimeout()
{
	qCritical() << "Localsettings not available!";
	::exit(EXIT_FAILURE);
}

void Application::onCanInterfacesChanged()
{
	mService->itemGetOrCreateAndProduce("CanBus/Interfaces", QVariant::fromValue(mCanInterfaceMonitor->canInfo()));
}

void Application::onMk3UpdateAllowedChanged(QVariant var)
{
	static QVariant lastValue;
	bool ok;

	if (lastValue.toInt(&ok) == 0 && ok && var.toInt(&ok) == 1 && ok) {
		qDebug() << "restarting all Vebus services";
		spawn("sh", QStringList() << "-c" << "svc -t /service/mk2-dbus.*");
	}

	lastValue = var;
}

void Application::onDemoSettingChanged(QVariant var)
{
	static bool isStarted = false;

	if (var.isValid()) {
		if (var.toInt() > 0) {
			isStarted = true;
			spawn(venusDir.filePath("dbus-recorder/startdemo.sh"),
				QStringList() << QString::number(var.toInt()));
		} else if (isStarted) {
			/*
			 * NOTE: isStarted prevents calling stopdemo during a normal boot
			 * without the demo mode enabled, since the stop script restarts
			 * normal services and that can interfere with the normal startup.
			 */
			isStarted = false;
			spawn(venusDir.filePath("dbus-recorder/stopdemo.sh"));
		}
	}
}

void Application::onRunningGuiVersionObtained(const QVariant &var)
{
	if (mRunningGui != var) {
		if (mRunningGui.isValid() && var.isValid())
			mGuiSwitcher->restart();
		mRunningGui = var;
		emit runningGuiVersionChanged();
	}
}

void Application::onServiceAdded(VeQItem *var)
{
	if (var->id().startsWith("com.victronenergy.genset") || var->id().startsWith("com.victronenergy.dcgenset")) {
		connect(var, SIGNAL(stateChanged(VeQItem::State)), SLOT(onGensetStateChanged(VeQItem::State)));
	} else if (var->id().startsWith("com.victronenergy.battery")) {
		// Need to look at the product id to decide if the parallel bms service needs to start.
		VeQItem *item = var->itemGetOrCreate("ProductId");
		item->getValueAndChanges(this, SLOT(onBatteryProductIdChanged(QVariant)));
	}
}

void Application::onGensetStateChanged(VeQItem::State state)
{
	VeQItem *item = static_cast<VeQItem *>(sender());
	if (state == VeQItem::State::Synchronized)
		mGeneratorStarterConditions << item->id();
	else
		mGeneratorStarterConditions.removeAll(item->id());
	manageGeneratorStartStop();
}

void Application::onBatteryProductIdChanged(QVariant var)
{
	VeQItem *item = static_cast<VeQItem *>(sender());
	VeQItem *parent = item->itemParent();
	if (var.isValid()) {
		int id = var.toInt();
		if (id == LYNX_BMS_500 || id == LYNX_BMS_500_NG || id == LYNX_BMS_1000 || id == LYNX_BMS_1000_NG)
			mParallelBmsConditions << parent->id();
		else
			mParallelBmsConditions.removeAll(parent->id());
	} else {
		mParallelBmsConditions.removeAll(parent->id());
	}
	manageParallelBms();
}

void Application::onRelaySettingChanged(QVariant var)
{
	if (var.isValid() && var.toInt() == 1)
		mGeneratorStarterConditions << "Relay";
	else
		mGeneratorStarterConditions.removeAll("Relay");
	manageGeneratorStartStop();
}

void Application::manageGeneratorStartStop()
{
	if (!mGeneratorStarter)
		return;
	if (!mGeneratorStarterConditions.empty() && !mGeneratorStarter->isUp())
		mGeneratorStarter->start();
	else if (mGeneratorStarterConditions.empty() && mGeneratorStarter->isUp())
		mGeneratorStarter->stop();
}

void Application::manageParallelBms()
{
	if (!mParallelBmsStarter)
		return;
	if (!mParallelBmsConditions.empty() && !mParallelBmsStarter->isUp())
		mParallelBmsStarter->start();
	else if (mParallelBmsConditions.empty() && mParallelBmsStarter->isUp())
		mParallelBmsStarter->stop();
}

void Application::initDaemonStartupConditions(VeQItem *service)
{
	if (service->getState() == VeQItem::State::Synchronized) {
		if (service->id().startsWith("com.victronenergy.genset") || service->id().startsWith("com.victronenergy.dcgenset")) {
			connect(service, SIGNAL(stateChanged(VeQItem::State)), SLOT(onGensetStateChanged(VeQItem::State)));
			mGeneratorStarterConditions << service->id();
		} else if (service->id().startsWith("com.victronenergy.battery")) {
			VeQItem *item = service->itemGetOrCreate("ProductId");
			item->getValueAndChanges(this, SLOT(onBatteryProductIdChanged(QVariant)));
		}
	}
}

void Application::manageDaemontoolsServices()
{
	// Is gui-v1 the only option
	if (QDir("/service/gui").exists()) {
		mRunningGui = 1;

	// or if configurable, is it running?
	} else {
		mGuiSwitcher = new DaemonToolsService("/service/start-gui");
		VeQItem *item = mSettings->root()->itemGetOrCreate("Settings/Gui/RunningVersion");
		item->getValueAndChanges(this, SLOT(onRunningGuiVersionObtained(QVariant)));
	}

	mParallelBmsStarter = new DaemonToolsService("/service/dbus-parallel-bms");
	mGeneratorStarter = new DaemonToolsService("/service/dbus-generator-starter");
	VeQItem *item = mSettings->root()->itemGetOrCreate("Settings/Relay/Function");
	item->getValueAndChanges(this, SLOT(onRelaySettingChanged(QVariant)));

	VeQItem::Children const children = mServices->itemChildren();
	for (VeQItem *child: children)
		initDaemonStartupConditions(child);

	connect(mServices, SIGNAL(childAdded(VeQItem*)), SLOT(onServiceAdded(VeQItem*)));
	manageGeneratorStartStop();

	new DaemonToolsService(mSettings, "/service/dbus-ble-sensors", "Settings/Services/BleSensors", this);

	new DaemonToolsService(mSettings, "/service/dbus-pump", "Settings/Relay/Function", 3, this);

	new DaemonToolsService(mSettings, "/service/dbus-modbustcp", "Settings/Services/Modbus",
						   this, QStringList() << "-s" << "dbus-modbustcp");

	// Temperature relay
	QList<QString> tempSensorRelayList = QList<QString>() << "Settings/Relay/Function" << "Settings/Relay/1/Function";
	new DaemonToolsService(mSettings, "/service/dbus-tempsensor-relay", tempSensorRelayList, 4, this, false);

	// Large image services
	if (serviceExists("node-red-venus")) {
		QList<int> start = QList<int>() << 1 << 2;
		new DaemonToolsService(mSettings, "/service/node-red-venus", "Settings/Services/NodeRed", start, this);
		VeQItemProxy::addProxy(mService->itemGetOrCreate("Services/NodeRed"), "Mode",
							   mSettings->root()->itemGetOrCreate("Settings/Services/NodeRed"));
	}

	if (serviceExists("signalk-server")) {
		new DaemonToolsService(mSettings, "/service/signalk-server", "Settings/Services/SignalK", this);
		VeQItemProxy::addProxy(mService->itemGetOrCreate("Services/SignalK"), "Enabled",
							   mSettings->root()->itemGetOrCreate("Settings/Services/SignalK"));
	}

	new DaemonToolsService(mSettings, "/service/vesmart-server", "Settings/Services/Bluetooth",
						   this, QStringList() << "-s" << "vesmart-server");

	item = mSettings->root()->itemGetOrCreate("Settings/Vebus/AllowMk3Fw212Update");
	item->getValueAndChanges(this, SLOT(onMk3UpdateAllowedChanged(QVariant)));

	if (templateExists("hostapd")) {
		VeQItemProxy::addProxy(mService->itemGetOrCreate("Services/AccessPoint"), "Enabled",
							   mSettings->root()->itemGetOrCreate("Settings/Services/AccessPoint"));
		new DaemonToolsService(mSettings, "/service/hostapd", "Settings/Services/AccessPoint",
							   this, QStringList() << "-s" << "hostapd");
	}

	/*
	 * On behalf of venus-access, since that one should be kept as simple as possible.
	 * This only handles the optional settings: venus-platform -> localsettings -> venus-access.
	 */
	if (QFile("/dev/ttyconsole").exists()) {
		VeQItemProxy::addProxy(mService->itemGetOrCreate("Services/Console"), "Enabled",
							   mSettings->root()->itemGetOrCreate("Settings/Services/Console"));
	}

	// CAN-bus debugging over tcp/ip
	new DaemonToolsService(mSettings, "/service/socketcand", "Settings/Services/Socketcand",
						   this, QStringList() << "-s" << "socketcand");

	// An optionally service, which can be installed by e.g. a pendrive.
	if (QDir("/data/evcc/service/").exists()) {
		VeQItem *item = mSettings->root()->itemGetOrCreate("Settings/Services/Evcc");
		item->getValueAndChanges(this, SLOT(onEvccSettingChanged(QVariant)));
	}

	// Tailscale
	mTailscaleBackend = new DaemonToolsService("/service/tailscale-backend");
	mTailscaleControl = new DaemonToolsService("/service/tailscale-control");

	item = mSettings->root()->itemGetOrCreate("Settings/Services/Tailscale/Enabled");
	item->getValueAndChanges(this, SLOT(onTailscaleSettingChanged(QVariant)));
}

void Application::init()
{
	QString installerVersion;
	QFile file("/data/venus/installer-version");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		file.readLine(); // version number
		file.readLine(); // Victron
		installerVersion = file.readLine();
	}

	// mk3 firmware 212 should not automatically be updated after a Venus update.
	// When shipped with a newer version it should be fine. Since the mk3 version is unknown
	// without the service being started (which will update it), check the installer date instead.
	enum Mk3Update mk3update = !installerVersion.isEmpty() && installerVersion >= "202210050000" ? NOT_APPLICABLE : DISALLOWED;

	qDebug() << "Creating settings";
	if (!mSettings->addSettings(SettingsInfo(mk3update))) {
		qCritical() << "Creating settings failed";
		::exit(EXIT_FAILURE);
	}

	// Load the correct translation file first of all. Note, the add settings call above
	// already obtained the selected language, hence this directly continues in the slot.
	VeQItem *lang = mSettings->root()->itemGetOrCreate("Settings/Gui/Language");
	lang->getValueAndChanges(this, SLOT(onLanguageChanged(QVariant)));
}

void Application::onLanguageChanged(QVariant var)
{
	if (!var.isValid())
		return;

	bool init = mLanguage.isNull();

	QString language = var.toString();
	if (mLanguage != language) {
		mLanguage = language;
		qDebug() << "Language changed to" << language;
		loadTranslation();
		emit languageChanged();
	}

	if (init)
		start();
}

void Application::loadTranslation()
{
	// Remove translation to get original english texts
	if (mLanguage == "en") {
		qApp->removeTranslator(&mTranslator);
		return;
	}

	QString qmFile(qApp->applicationDirPath() + "/translations/venus_" + venusPoEditorLanguage(mLanguage) + ".qm");
	if (mTranslator.load(qmFile))
		qApp->installTranslator(&mTranslator);
	else
		qCritical() << "Failed to load translation file: " << qmFile;
}

void Application::start()
{
	// The items exported to the dbus..
	VeQItemProducer *toDbus = new VeQItemProducer(VeQItems::getRoot(), "to-dbus", this);
	mService = toDbus->services()->itemGetOrCreate("com.victronenergy.platform", false);

	mService->itemGetOrCreateAndProduce("ProductName", "Venus");

	manageDaemontoolsServices();

	mCanInterfaceMonitor = new CanInterfaceMonitor(mSettings, mService, this);
	connect(mCanInterfaceMonitor, SIGNAL(interfacesChanged()), SLOT(onCanInterfacesChanged()));
	mCanInterfaceMonitor->enumerate();

	mDisplayController = new DisplayController(mSettings, this);

	mUpdater = new Updater(mService, this);
	mLedController = new LedController(this);

	VeQItem *ledEnableSetting = mSettings->root()->itemGetOrCreate("Settings/LEDs/Enable");
	ledEnableSetting->getValueAndChanges(mLedController, SLOT(ledSettingChanged(QVariant)));

	VeQItem *bluetoothSetting = mSettings->root()->itemGet("Settings/Services/Bluetooth");
	if (bluetoothSetting)
		bluetoothSetting->getValueAndChanges(mLedController, SLOT(dbusSettingChanged()));

	VeQItem *accessPointSetting = mSettings->root()->itemGet("Settings/Services/AccessPoint");
	if (accessPointSetting)
		accessPointSetting->getValueAndChanges(mLedController, SLOT(dbusSettingChanged()));

	// Network controller
	mNetworkController = new NetworkController(mService, this);

	mService->itemGetOrCreate("Device")->itemAddChild("Reboot", new VeQItemReboot());
	mService->itemGetOrCreate("Device")->itemAddChild("Time", new VeQItemTime());

	QProcess *proc = Application::spawn("get-unique-id");
	proc->waitForFinished();
	mService->itemGetOrCreateAndProduce("Device/UniqueId", QString(proc->readAllStandardOutput().trimmed()));

	int error = dataPartionError() ? 1 : 0;
	mService->itemGetOrCreateAndProduce("Device/DataPartitionError", error);

	bool evccInstalled = QDir("/data/evcc/service/").exists();
	mService->itemGetOrCreateAndProduce("Services/Evcc/Installed", evccInstalled);

	// Demo mode
	VeQItem *demoModeSetting = mSettings->root()->itemGetOrCreate("Settings/Gui/DemoMode");
	demoModeSetting->getValueAndChanges(this, SLOT(onDemoSettingChanged(QVariant)));

	// Notifications
	mNotifications = new Notifications(mService, this);
	mVenusServices = new VenusServices(mServices, this);
	mAlarmBusitems = new AlarmBusitems(mVenusServices, mNotifications);

	new SecurityProfiles(mService, mSettings, mVenusServices, this);

	// Handle buzer and relay alarms
	mAudibleAlarm = mSettings->root()->itemGetOrCreate("Settings/Alarm/Audible");
	mAlarm = mService->itemGetOrCreate("/Notifications/Alarm");
	mBuzzer = new Buzzer("dbus/com.victronenergy.system/Buzzer/State");
	mAlarm->getValueAndChanges(this, SLOT(onAlarmChanged(QVariant)));
	mAudibleAlarm->getValueAndChanges(this, SLOT(onAlarmChanged(QVariant)));

	mRelay = new Relay("dbus/com.victronenergy.system/Relay/0/State", mNotifications, this);

	// Scan for dbus services
	mVenusServices->initialScan();

	// With everything ready, do export the service to the dbus
	VeQItemExportedDbusServices *publisher = new VeQItemExportedDbusServices(toDbus->services(), this);
	mService->produceValue(QString());
	publisher->open(VeDbusConnection::getDBusAddress());
}

QProcess *Application::spawn(QString const &cmd, const QStringList &args)
{
	QProcess *proc = new QProcess();
	connect(proc, SIGNAL(finished(int)), proc, SLOT(deleteLater()));
	proc->start(cmd, args);
	return proc;
}

int Application::run(QString const &cmd, const QStringList &args)
{
	QProcess proc;
	proc.start(cmd, args);
	proc.waitForFinished();
	return proc.exitCode();
}

bool Application::silenceBuzzer()
{
	if (mBuzzer->isBeeping()) {
		qDebug() << "Platform::silenceBuzzer";
		mBuzzer->buzzerOff();
		return true;
	}

	return false;
}

void Application::onAlarmChanged(QVariant var)
{
	Q_UNUSED(var);
	if (mAlarm->getValue().toBool()) {
		if (mAudibleAlarm->getValue().isValid() && mAudibleAlarm->getValue().toBool())
			mBuzzer->buzzerOn();
		else
			mBuzzer->buzzerOff();
	} else {
		mBuzzer->buzzerOff();
	}
}

void Application::onEvccSettingChanged(QVariant var)
{
	if (!var.isValid())
		return;

	if (var.toBool()) {
		qDebug() << "[Service] Enabling evcc";
		QFile::link("/data/evcc/service/", "/service/evcc");
		system("svc -u /service/evcc");
	} else {
		if (QDir("/service/evcc").exists()) {
			qDebug() << "[Service] Removing evcc";
			system("svc -d /service/evcc");
			QFile::remove("/service/evcc");
		}
	}
}

void Application::onTailscaleSettingChanged(QVariant var)
{
	if (!var.isValid())
		return;

	if (var.toBool()) {
		qDebug() << "[Service] Enabling tailscale-backend and tailscale-control";
		mTailscaleBackend->start();
		mTailscaleControl->start();
	} else {
		qDebug() << "[Service] Disabling tailscale-backend and tailscale-control";
		system("/usr/bin/tailscale down");
		qDebug() << "[Service] executed \"/usr/bin/tailscale down\"";
		mTailscaleBackend->stop();
		mTailscaleControl->stop();
	}
}

bool Application::setRootPassword(QString password)
{
	QProcess process;
	process.start(venusDir.filePath("swupdate-scripts/resize2fs.sh"));
	process.waitForFinished();

	process.start("/usr/sbin/chpasswd");
	process.waitForStarted();

	QString line("root:" + password);
	process.write(line.toLocal8Bit());
	process.closeWriteChannel();
	process.waitForFinished();

	if (process.exitCode() == 0) {
		qWarning() << "Root password changed";
		return true;
	}

	qCritical() << "Changing password failed";
	return false;
}

// After e.g. a password change at make sure persistent logins are
// invalidated and need to re-authenticate.
void Application::invalidateAuthenticatedSessions()
{
	QDir const dir("/data/var/lib/venus-www-sessions");
	if (!dir.exists()) {
		qWarning() << dir.absolutePath() << "doesn't exist";
		return;
	}

	QFileInfoList const entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files);
	for (QFileInfo const &info: entries) {
		if (!QFile::remove(info.absoluteFilePath()))
			qWarning() << "failed to remove" << info.absoluteFilePath();
	}
}
