/******************************************************************************
Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <wchar.h>
#include <chrono>
#include <ratio>
#include <sstream>
#include <mutex>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <obs-config.h>
#include <jansson.h>
#include <obs.hpp>

#include <QProxyStyle>

#include "qt-wrappers.hpp"
#include "obs-app.hpp"
#include "window-basic-main.hpp"
#include "window-basic-settings.hpp"
#include "window-license-agreement.hpp"
#include "crash-report.hpp"
#include "platform.hpp"
#include <QDir>
#include <QInputDialog>
#include <QMessageBox>
#include <QTextCodec>

#include <fstream>

#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

using namespace std;

static log_handler_t def_log_handler;

static string currentLogFile;
static string lastLogFile;

static bool portable_mode = false;
bool opt_start_streaming = false;
bool opt_start_recording = false;
string opt_starting_collection;
string opt_starting_profile;
string opt_starting_scene;
char *access_token_global;

void CodeWinHandler::check_input(const QString & text)
{
	QPushButton *button = (QPushButton*)button_data;
	if (input->hasAcceptableInput())
		button->setDisabled(false);
	else
		button->setDisabled(true);
}

char* prompt_captcha(json_t *root, QDialog *loginWin)                                                                                                            
{
	QDialog *window = new QDialog(loginWin,
		Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	window->setPalette(loginWin->palette());
	window->setFixedSize(321, 182);
	window->setWindowTitle(QTStr("Vk.Title"));

	QFormLayout form(window);
	form.setFormAlignment(Qt::AlignVCenter);

	const char *url = json_string_value(json_object_get(root,
		"captcha_img"));
	char *imgdata;
	size_t size = get_captchaimg(url, &imgdata);
	QPixmap *img = new QPixmap();
	img->loadFromData((uchar*)imgdata, size, "jpeg");
	free(imgdata);
	QLabel *img_label = new QLabel();
	img_label->setAlignment(Qt::AlignHCenter);
	img_label->setPixmap(*img);
	form.addRow(img_label);

	QLineEdit *inputField = new QLineEdit(window);
	inputField->setStyleSheet("padding-left: 1ex;");
	inputField->setPlaceholderText(QTStr("Vk.Captcha"));
	inputField->setFixedSize(175, 30);
	QFont font("Segoe UI", 10);
	inputField->setFont(font);
	form.addRow(inputField);
	form.setAlignment(inputField, Qt::AlignHCenter);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok
		| QDialogButtonBox::Cancel);
	buttonBox->setStyleSheet(
		"QPushButton { \
			background-color: #6888ad;\
			color: white;\
			border: none;\
		}\
		QPushButton:pressed { \
			background-color:#507299\
		}");
	buttonBox->button(QDialogButtonBox::Ok)->setFixedSize(87, 22);
	buttonBox->button(QDialogButtonBox::Cancel)->setFixedSize(87, 22);
	buttonBox->setFont(font);
	QObject::connect(buttonBox, SIGNAL(accepted()), window, SLOT(accept()));
	QObject::connect(buttonBox, SIGNAL(rejected()), window, SLOT(reject()));
	form.addRow(buttonBox);
	form.setAlignment(buttonBox, Qt::AlignHCenter);
	if (window->exec() == QDialog::Rejected)
		return NULL;
	QByteArray guess_qarray = inputField->text().toUtf8();
	return strdup(guess_qarray.data());
}

enum codetype{ retry, app, sms };
char *prompt_code(codetype type, QDialog *loginWin)
{
	QDialog *window = new QDialog(loginWin,
		Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	window->setFixedSize(248, 126);
	window->setPalette(loginWin->palette());
	QFormLayout form(window);
	form.setFormAlignment(Qt::AlignVCenter);
	CodeWinHandler *validate = new CodeWinHandler();
	window->setWindowTitle(QTStr("Vk.Title"));

	QString subTitleText;
	QString background;
	if (type == retry) {
		subTitleText = QTStr("Vk.VerifyFailed");
		background = "";
	} else if (type == app) {
		subTitleText = QTStr("Vk.VerifyLogin");
		background = QTStr("Vk.UseApp");
	} else if (type == sms) {
		subTitleText = QTStr("Vk.VerifyLogin");
		background = QTStr("Vk.UseSMS");
	}
	QLabel *subTitle = new QLabel(subTitleText);
	subTitle->setAlignment(Qt::AlignHCenter);
	QFont font("Segoe UI", 10);
	subTitle->setFont(font);
	form.addRow(subTitle);
	QLineEdit *edit = new QLineEdit(window);
	edit->setStyleSheet("padding-left: 1ex;");
	edit->setPlaceholderText(background);
	edit->setFixedSize(175, 30);
	edit->setFont(font);
	QRegExp rx("\\d{6,8}");
	QValidator *validator = new QRegExpValidator(rx);
	edit->setValidator(validator);
	form.addRow(edit);
	form.setAlignment(edit, Qt::AlignHCenter);
	QDialogButtonBox *buttonBox = new QDialogButtonBox();
	QPushButton *okButton = new QPushButton(QTStr("OK"));
	okButton->setStyleSheet(
		"QPushButton { \
			background-color: #6888ad;\
			color: white;\
			border: none;\
		}\
		QPushButton:pressed { \
			background-color:#507299\
		}");
	okButton->setFixedSize(87, 22);
	okButton->setFont(font);
	buttonBox->addButton(okButton, QDialogButtonBox::AcceptRole);
	buttonBox->setCenterButtons(true);
	form.addRow(buttonBox);
	QObject::connect(buttonBox, SIGNAL(accepted()), window, SLOT(accept()));
	validate->input = edit;
	validate->button_data = okButton;
	QObject::connect(edit, SIGNAL(textEdited(QString)), validate,
		SLOT(check_input(QString)));
	okButton->setDisabled(true);

	if (window->exec() == QDialog::Rejected)
		return NULL;
	QByteArray code_qarray = edit->text().toUtf8();
	return strdup(code_qarray.data());
}

void show_pretty_message(QWidget *parent, QString message)
{
	QDialog *window = new QDialog(parent,
		Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	window->setWindowTitle(QTStr("Vk.Title"));
	//window->setFixedSize(275, 130);
	window->setPalette(parent->palette());
	QFormLayout form(window);
	form.setFormAlignment(Qt::AlignVCenter);
	QFont font("Segoe UI", 10);

	QLabel *messageLabel = new QLabel(message);
	messageLabel->setAlignment(Qt::AlignHCenter);
	messageLabel->setFont(font);
	form.addRow(new QLabel());
	form.addRow(messageLabel);
	form.addRow(new QLabel());
	QDialogButtonBox *buttonBox = new QDialogButtonBox();
	QPushButton *okButton = new QPushButton(QTStr("OK"));
	okButton->setStyleSheet(
		"QPushButton { \
			background-color: #6888ad;\
			color: white;\
			border: none;\
		}\
		QPushButton:pressed { \
			background-color:#507299\
		}");
	okButton->setFixedSize(87, 22);
	okButton->setFont(font);
	buttonBox->addButton(okButton, QDialogButtonBox::AcceptRole);
	buttonBox->setCenterButtons(true);
	form.addRow(buttonBox);
	form.addRow(new QLabel());
	QObject::connect(buttonBox, SIGNAL(accepted()), window, SLOT(accept()));

	window->exec();

}

void LoginWinHandler::process_login()
{
	login_data_t login;
	captcha_data_t captcha;
	query_param_t code;
	json_t *response;

	login.username.name = "username";
	QByteArray username_qarray = usernameField->text().toUtf8();
	login.username.value = username_qarray.data();

	login.password.name = "password";
	QByteArray password_qarray = passwordField->text().toUtf8();
	login.password.value = password_qarray.data();

	code.name = "code";
	captcha.sid.name = "captcha_sid";
	captcha.key.name = "captcha_key";


	response = simple_login(&login);
process_response:
	switch (get_loginstatus(response)) {
	case not_json:
		show_pretty_message(loginWin, QTStr("Vk.InvalidJson"));
		break;
	case invalid_client:
		show_pretty_message(loginWin, QTStr("Vk.InvalidLogin"));
		break;
	case need_captcha:
		captcha.key.value = prompt_captcha(response, loginWin);
		if (!captcha.key.value)
			break;
		captcha.sid.value = strdup(json_string_value(json_object_get(
			response, "captcha_sid")));
		json_decref(response);
		response = login_w_captcha(&login, &captcha);
		free(captcha.key.value);
		free(captcha.sid.value);
		goto process_response;
	case need_appcode:
		code.value = prompt_code(app, loginWin);
		if (!code.value)
			break;
		json_decref(response);
		response = login_w_code(&login, code);
		free(code.value);
		goto process_response;
	case need_smscode:
		code.value = prompt_code(sms, loginWin);
		if (!code.value)
			break;
		json_decref(response);
		response = login_w_code(&login, code);
		free(code.value);
		goto process_response;
	case wrong_code:
		code.value = prompt_code(retry, loginWin);
		if (!code.value)
			break;
		json_decref(response);
		response = login_w_code(&login, code);
		free(code.value);
		goto process_response;
	case ok:
		access_token_global = strdup(json_string_value(json_object_get(
			response, "access_token")));
		config_set_string(App()->GlobalConfig(),
			"Authentication", "Token", access_token_global);
		token_received();
		break;
	}
	json_decref(response);
}

bool enter_vk()
{
	LoginWinHandler *login = new LoginWinHandler();

	QDialog *login_window = new QDialog(NULL,
		Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	QPalette pal;
	pal.setColor(QPalette::Window, "#FFFFFF");
	login_window->setAutoFillBackground(true);
	login_window->setPalette(pal);
	login_window->resize(701, 414);
	QFormLayout login_form(login_window);
	//login_form.setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
	login_form.setFormAlignment(Qt::AlignVCenter);
	//login_form.setLabelAlignment(Qt::AlignHCenter);
	login_window->setWindowTitle(QTStr("Vk.Title"));

	/*QPixmap logo;
	logo.load("../obsStudio64.bmp");
	QLabel *logo_label = new QLabel();
	logo_label->setAlignment(Qt::AlignHCenter);
	logo_label->setPixmap(logo);
	login_form.addRow(logo_label);
	*/QLabel *subTitle = new QLabel(QTStr("Vk.SubTitle"));
	QFont *font = new QFont("Segoe UI Light", 18);
	subTitle->setFont(*font);
	/*pal.setColor(QPalette::WindowText, Qt::GlobalColor::white);
	subTitle->setPalette(pal);
	*/subTitle->setAlignment(Qt::AlignHCenter);
	login_form.addRow(subTitle);
	login_form.addRow(new QLabel());
	login_form.addRow(new QLabel());
	QLabel *loginPlease = new QLabel(QTStr("Vk.LoginPlease"));
	font->setPointSize(13);
	loginPlease->setFont(*font);
	//loginPlease->setPalette(pal);
	loginPlease->setAlignment(Qt::AlignHCenter);
	login_form.addRow(loginPlease);
	login_form.addRow(new QLabel());
	QLineEdit *usernameField = new QLineEdit(login_window);
	//usernameField->setSizePolicy(QSizePolicy::QSizePolicy());
	usernameField->setPlaceholderText(QTStr("Vk.Username"));
	usernameField->setFixedSize(270, 30);
	font->setPointSize(10);
	font->setFamily("Segoe UI");
	usernameField->setFont(*font);
	usernameField->setStyleSheet("padding-left: 1ex;");
	QLineEdit *passwordField = new QLineEdit(login_window);
	//passwordField->setSizePolicy(QSizePolicy::QSizePolicy());
	passwordField->setPlaceholderText(QTStr("Vk.Password"));
	passwordField->setFixedSize(270, 30);
	passwordField->setFont(*font);
	passwordField->setStyleSheet("padding-left: 1ex;");
	passwordField->setEchoMode(QLineEdit::Password);
	login_form.addRow(usernameField);
	login_form.addRow(passwordField);
	login_form.setAlignment(usernameField, Qt::AlignHCenter);
	login_form.setAlignment(passwordField, Qt::AlignHCenter);
	QPushButton *button = new QPushButton(QTStr("Vk.Enter"), login_window);
	login->usernameField = usernameField;
	login->passwordField = passwordField;
	login->loginWin = login_window;
	QObject::connect(button, SIGNAL(clicked()),
		login, SLOT(process_login()));
	QObject::connect(login, SIGNAL(token_received()),
		login_window, SLOT(accept()));
	//button->setSizePolicy(QSizePolicy::QSizePolicy());
	button->setStyleSheet(
		"QPushButton { \
			background-color: #6888ad;\
			color: white;\
			border: none;\
		}\
		QPushButton:pressed { \
			background-color:#507299\
		}");
	button->setFixedSize(270, 30);
	button->setFont(*font);
	login_form.addRow(button);
	login_form.addRow(new QLabel());
	login_form.addRow(new QLabel());
	QLabel *calming_text = new QLabel(QTStr("Vk.Console"));
	font->setFamily("Segoe UI Light");
	calming_text->setFont(*font);
	calming_text->setAlignment(Qt::AlignHCenter);
	login_form.addRow(calming_text);
	login_form.setAlignment(button, Qt::AlignHCenter);
	if (login_window->exec() == QDialog::Rejected)
		return false;
	return true;
}

QObject *CreateShortcutFilter()
{
	return new OBSEventFilter([](QObject *obj, QEvent *event)
	{
		auto mouse_event = [](QMouseEvent &event)
		{
			obs_key_combination_t hotkey = { 0, OBS_KEY_NONE };
			bool pressed = event.type() == QEvent::MouseButtonPress;

			switch (event.button()) {
			case Qt::NoButton:
			case Qt::LeftButton:
			case Qt::RightButton:
			case Qt::AllButtons:
			case Qt::MouseButtonMask:
				return false;

			case Qt::MidButton:
				hotkey.key = OBS_KEY_MOUSE3;
				break;

#define MAP_BUTTON(i, j) case Qt::ExtraButton ## i: \
		hotkey.key = OBS_KEY_MOUSE ## j; break;
				MAP_BUTTON(1, 4);
				MAP_BUTTON(2, 5);
				MAP_BUTTON(3, 6);
				MAP_BUTTON(4, 7);
				MAP_BUTTON(5, 8);
				MAP_BUTTON(6, 9);
				MAP_BUTTON(7, 10);
				MAP_BUTTON(8, 11);
				MAP_BUTTON(9, 12);
				MAP_BUTTON(10, 13);
				MAP_BUTTON(11, 14);
				MAP_BUTTON(12, 15);
				MAP_BUTTON(13, 16);
				MAP_BUTTON(14, 17);
				MAP_BUTTON(15, 18);
				MAP_BUTTON(16, 19);
				MAP_BUTTON(17, 20);
				MAP_BUTTON(18, 21);
				MAP_BUTTON(19, 22);
				MAP_BUTTON(20, 23);
				MAP_BUTTON(21, 24);
				MAP_BUTTON(22, 25);
				MAP_BUTTON(23, 26);
				MAP_BUTTON(24, 27);
#undef MAP_BUTTON
			}

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(
				event.modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		auto key_event = [&](QKeyEvent *event)
		{
			QDialog *dialog = qobject_cast<QDialog*>(obj);

			obs_key_combination_t hotkey = { 0, OBS_KEY_NONE };
			bool pressed = event->type() == QEvent::KeyPress;

			switch (event->key()) {
			case Qt::Key_Shift:
			case Qt::Key_Control:
			case Qt::Key_Alt:
			case Qt::Key_Meta:
				break;

#ifdef __APPLE__
			case Qt::Key_CapsLock:
				// kVK_CapsLock == 57
				hotkey.key = obs_key_from_virtual_key(57);
				pressed = true;
				break;
#endif

			case Qt::Key_Enter:
			case Qt::Key_Escape:
			case Qt::Key_Return:
				if (dialog && pressed)
					return false;
			default:
				hotkey.key = obs_key_from_virtual_key(
					event->nativeVirtualKey());
			}

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(
				event->modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
			return mouse_event(*static_cast<QMouseEvent*>(event));

			/*case QEvent::MouseButtonDblClick:
			case QEvent::Wheel:*/
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return key_event(static_cast<QKeyEvent*>(event));

		default:
			return false;
		}
	});
}

string CurrentTimeString()
{
	using namespace std::chrono;

	struct tm  tstruct;
	char       buf[80];

	auto tp = system_clock::now();
	auto now = system_clock::to_time_t(tp);
	tstruct = *localtime(&now);

	size_t written = strftime(buf, sizeof(buf), "%X", &tstruct);
	if (ratio_less<system_clock::period, seconds::period>::value &&
		written && (sizeof(buf) - written) > 5) {
		auto tp_secs =
			time_point_cast<seconds>(tp);
		auto millis =
			duration_cast<milliseconds>(tp - tp_secs).count();

		snprintf(buf + written, sizeof(buf) - written, ".%03u",
			static_cast<unsigned>(millis));
	}

	return buf;
}

string CurrentDateTimeString()
{
	time_t     now = time(0);
	struct tm  tstruct;
	char       buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%Y-%m-%d, %X", &tstruct);
	return buf;
}

static inline void LogString(fstream &logFile, const char *timeString,
	char *str)
{
	logFile << timeString << str << endl;
}

static inline void LogStringChunk(fstream &logFile, char *str)
{
	char *nextLine = str;
	string timeString = CurrentTimeString();
	timeString += ": ";

	while (*nextLine) {
		char *nextLine = strchr(str, '\n');
		if (!nextLine)
			break;

		if (nextLine != str && nextLine[-1] == '\r') {
			nextLine[-1] = 0;
		}
		else {
			nextLine[0] = 0;
		}

		LogString(logFile, timeString.c_str(), str);
		nextLine++;
		str = nextLine;
	}

	LogString(logFile, timeString.c_str(), str);
}

#define MAX_REPEATED_LINES 30
#define MAX_CHAR_VARIATION (255 * 3)

static inline int sum_chars(const char *str)
{
	int val = 0;
	for (; *str != 0; str++)
		val += *str;

	return val;
}

static inline bool too_many_repeated_entries(fstream &logFile, const char *msg,
	const char *output_str)
{
	static mutex log_mutex;
	static const char *last_msg_ptr = nullptr;
	static int last_char_sum = 0;
	static char cmp_str[4096];
	static int rep_count = 0;

	int new_sum = sum_chars(output_str);

	lock_guard<mutex> guard(log_mutex);

	if (last_msg_ptr == msg) {
		int diff = std::abs(new_sum - last_char_sum);
		if (diff < MAX_CHAR_VARIATION) {
			return (rep_count++ >= MAX_REPEATED_LINES);
		}
	}

	if (rep_count > MAX_REPEATED_LINES) {
		logFile << CurrentTimeString() <<
			": Last log entry repeated for " <<
			to_string(rep_count - MAX_REPEATED_LINES) <<
			" more lines" << endl;
	}

	last_msg_ptr = msg;
	strcpy(cmp_str, output_str);
	last_char_sum = new_sum;
	rep_count = 0;

	return false;
}

static void do_log(int log_level, const char *msg, va_list args, void *param)
{
	fstream &logFile = *static_cast<fstream*>(param);
	char str[4096];

#ifndef _WIN32
	va_list args2;
	va_copy(args2, args);
#endif

	vsnprintf(str, 4095, msg, args);

#ifdef _WIN32
	OutputDebugStringA(str);
	OutputDebugStringA("\n");
#else
	def_log_handler(log_level, msg, args2, nullptr);
#endif

	if (too_many_repeated_entries(logFile, msg, str))
		return;

	if (log_level <= LOG_INFO)
		LogStringChunk(logFile, str);

#if defined(_WIN32) && defined(OBS_DEBUGBREAK_ON_ERROR)
	if (log_level <= LOG_ERROR && IsDebuggerPresent())
		__debugbreak();
#endif
}

#define DEFAULT_LANG "en-US"

bool OBSApp::InitGlobalConfigDefaults()
{
	config_set_default_string(globalConfig, "General", "Language",
		DEFAULT_LANG);
	config_set_default_uint(globalConfig, "General", "MaxLogs", 10);
	config_set_default_string(globalConfig, "General", "ProcessPriority",
		"Normal");

#if _WIN32
	config_set_default_string(globalConfig, "Video", "Renderer",
		"Direct3D 11");
#else
	config_set_default_string(globalConfig, "Video", "Renderer", "OpenGL");
#endif

	config_set_default_bool(globalConfig, "BasicWindow", "PreviewEnabled",
		true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"PreviewProgramMode", false);
	config_set_default_bool(globalConfig, "BasicWindow",
		"SceneDuplicationMode", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"SwapScenesMode", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"SnappingEnabled", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"ScreenSnapping", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"SourceSnapping", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"CenterSnapping", false);
	config_set_default_double(globalConfig, "BasicWindow",
		"SnapDistance", 10.0);
	config_set_default_bool(globalConfig, "BasicWindow",
		"RecordWhenStreaming", false);
	config_set_default_bool(globalConfig, "BasicWindow",
		"KeepRecordingWhenStreamStops", false);
	config_set_default_bool(globalConfig, "BasicWindow",
		"ShowTransitions", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"ShowListboxToolbars", true);
	config_set_default_bool(globalConfig, "BasicWindow",
		"ShowStatusBar", true);

#ifdef __APPLE__
	config_set_default_bool(globalConfig, "Video", "DisableOSXVSync", true);
	config_set_default_bool(globalConfig, "Video", "ResetOSXVSyncOnExit",
		true);
#endif
	return true;
}

static bool do_mkdir(const char *path)
{
	if (os_mkdirs(path) == MKDIR_ERROR) {
		OBSErrorBox(NULL, "Failed to create directory %s", path);
		return false;
	}

	return true;
}

static bool MakeUserDirs()
{
	char path[512];

	if (GetConfigPath(path, sizeof(path), "vk-games/basic") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

	if (GetConfigPath(path, sizeof(path), "vk-games/logs") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

	if (GetConfigPath(path, sizeof(path), "vk-games/profiler_data") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

#ifdef _WIN32
	if (GetConfigPath(path, sizeof(path), "vk-games/crashes") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;
#endif
	if (GetConfigPath(path, sizeof(path), "vk-games/plugin_config") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

	return true;
}

static bool MakeUserProfileDirs()
{
	char path[512];

	if (GetConfigPath(path, sizeof(path), "vk-games/basic/profiles") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

	if (GetConfigPath(path, sizeof(path), "vk-games/basic/scenes") <= 0)
		return false;
	if (!do_mkdir(path))
		return false;

	return true;
}

static string GetProfileDirFromName(const char *name)
{
	string outputPath;
	os_glob_t *glob;
	char path[512];

	if (GetConfigPath(path, sizeof(path), "vk-games/basic/profiles") <= 0)
		return outputPath;

	strcat(path, "/*.*");

	if (os_glob(path, 0, &glob) != 0)
		return outputPath;

	for (size_t i = 0; i < glob->gl_pathc; i++) {
		struct os_globent ent = glob->gl_pathv[i];
		if (!ent.directory)
			continue;

		strcpy(path, ent.path);
		strcat(path, "/basic.ini");

		ConfigFile config;
		if (config.Open(path, CONFIG_OPEN_EXISTING) != 0)
			continue;

		const char *curName = config_get_string(config, "General",
			"Name");
		if (astrcmpi(curName, name) == 0) {
			outputPath = ent.path;
			break;
		}
	}

	os_globfree(glob);

	if (!outputPath.empty()) {
		replace(outputPath.begin(), outputPath.end(), '\\', '/');
		const char *start = strrchr(outputPath.c_str(), '/');
		if (start)
			outputPath.erase(0, start - outputPath.c_str() + 1);
	}

	return outputPath;
}

static string GetSceneCollectionFileFromName(const char *name)
{
	string outputPath;
	os_glob_t *glob;
	char path[512];

	if (GetConfigPath(path, sizeof(path), "vk-games/basic/scenes") <= 0)
		return outputPath;

	strcat(path, "/*.json");

	if (os_glob(path, 0, &glob) != 0)
		return outputPath;

	for (size_t i = 0; i < glob->gl_pathc; i++) {
		struct os_globent ent = glob->gl_pathv[i];
		if (ent.directory)
			continue;

		obs_data_t *data =
			obs_data_create_from_json_file_safe(ent.path, "bak");
		const char *curName = obs_data_get_string(data, "name");

		if (astrcmpi(name, curName) == 0) {
			outputPath = ent.path;
			obs_data_release(data);
			break;
		}

		obs_data_release(data);
	}

	os_globfree(glob);

	if (!outputPath.empty()) {
		outputPath.resize(outputPath.size() - 5);
		replace(outputPath.begin(), outputPath.end(), '\\', '/');
		const char *start = strrchr(outputPath.c_str(), '/');
		if (start)
			outputPath.erase(0, start - outputPath.c_str() + 1);
	}

	return outputPath;
}

bool OBSApp::InitGlobalConfig()
{
	char path[512];

	int len = GetConfigPath(path, sizeof(path),
		"vk-games/global.ini");
	if (len <= 0) {
		return false;
	}

	int errorcode = globalConfig.Open(path, CONFIG_OPEN_ALWAYS);
	if (errorcode != CONFIG_SUCCESS) {
		OBSErrorBox(NULL, "Failed to open global.ini: %d", errorcode);
		return false;
	}

	if (!opt_starting_collection.empty()) {
		string path = GetSceneCollectionFileFromName(
			opt_starting_collection.c_str());
		if (!path.empty()) {
			config_set_string(globalConfig,
				"Basic", "SceneCollection",
				opt_starting_collection.c_str());
			config_set_string(globalConfig,
				"Basic", "SceneCollectionFile",
				path.c_str());
		}
	}

	if (!opt_starting_profile.empty()) {
		string path = GetProfileDirFromName(
			opt_starting_profile.c_str());
		if (!path.empty()) {
			config_set_string(globalConfig, "Basic", "Profile",
				opt_starting_profile.c_str());
			config_set_string(globalConfig, "Basic", "ProfileDir",
				path.c_str());
		}
	}

	return InitGlobalConfigDefaults();
}

bool OBSApp::InitLocale()
{
	ProfileScope("OBSApp::InitLocale");
	const char *lang = config_get_string(globalConfig, "General",
		"Language");

	locale = lang;

	string englishPath;
	if (!GetDataFilePath("locale/" DEFAULT_LANG ".ini", englishPath)) {
		OBSErrorBox(NULL, "Failed to find locale/" DEFAULT_LANG ".ini");
		return false;
	}

	textLookup = text_lookup_create(englishPath.c_str());
	if (!textLookup) {
		OBSErrorBox(NULL, "Failed to create locale from file '%s'",
			englishPath.c_str());
		return false;
	}

	bool userLocale = config_has_user_value(globalConfig, "General",
		"Language");
	bool defaultLang = astrcmpi(lang, DEFAULT_LANG) == 0;

	if (userLocale && defaultLang)
		return true;

	if (!userLocale && defaultLang) {
		for (auto &locale_ : GetPreferredLocales()) {
			if (locale_ == lang)
				return true;

			stringstream file;
			file << "locale/" << locale_ << ".ini";

			string path;
			if (!GetDataFilePath(file.str().c_str(), path))
				continue;

			if (!text_lookup_add(textLookup, path.c_str()))
				continue;

			blog(LOG_INFO, "Using preferred locale '%s'",
				locale_.c_str());
			locale = locale_;
			return true;
		}

		return true;
	}

	stringstream file;
	file << "locale/" << lang << ".ini";

	string path;
	if (GetDataFilePath(file.str().c_str(), path)) {
		if (!text_lookup_add(textLookup, path.c_str()))
			blog(LOG_ERROR, "Failed to add locale file '%s'",
				path.c_str());
	}
	else {
		blog(LOG_ERROR, "Could not find locale file '%s'",
			file.str().c_str());
	}

	return true;
}

bool OBSApp::SetTheme(std::string name, std::string path)
{
	theme = name;

	/* Check user dir first, then preinstalled themes. */
	if (path == "") {
		char userDir[512];
		name = "themes/" + name + ".qss";
		string temp = "obs-studio/" + name;
		int ret = GetConfigPath(userDir, sizeof(userDir),
			temp.c_str());

		if (ret > 0 && QFile::exists(userDir)) {
			path = string(userDir);
		}
		else if (!GetDataFilePath(name.c_str(), path)) {
			OBSErrorBox(NULL, "Failed to find %s.", name.c_str());
			return false;
		}
	}

	QString mpath = QString("file:///") + path.c_str();
	setStyleSheet(mpath);
	return true;
}

bool OBSApp::InitTheme()
{
	const char *themeName = config_get_string(globalConfig, "General",
		"Theme");

	if (!themeName)
		themeName = "Default";

	stringstream t;
	t << themeName;
	return SetTheme(t.str());
}

OBSApp::OBSApp(int &argc, char **argv, profiler_name_store_t *store)
	: QApplication(argc, argv),
	profilerNameStore(store)
{
	sleepInhibitor = os_inhibit_sleep_create("OBS Video/audio");
}

OBSApp::~OBSApp()
{
#ifdef __APPLE__
	bool vsyncDiabled = config_get_bool(globalConfig, "Video",
		"DisableOSXVSync");
	bool resetVSync = config_get_bool(globalConfig, "Video",
		"ResetOSXVSyncOnExit");
	if (vsyncDiabled && resetVSync)
		EnableOSXVSync(true);
#endif

	os_inhibit_sleep_set_active(sleepInhibitor, false);
	os_inhibit_sleep_destroy(sleepInhibitor);
}

static void move_basic_to_profiles(void)
{
	char path[512];
	char new_path[512];
	os_glob_t *glob;

	/* if not first time use */
	if (GetConfigPath(path, 512, "vk-games/basic") <= 0)
		return;
	if (!os_file_exists(path))
		return;

	/* if the profiles directory doesn't already exist */
	if (GetConfigPath(new_path, 512, "vk-games/basic/profiles") <= 0)
		return;
	if (os_file_exists(new_path))
		return;

	if (os_mkdir(new_path) == MKDIR_ERROR)
		return;

	strcat(new_path, "/");
	strcat(new_path, Str("Untitled"));
	if (os_mkdir(new_path) == MKDIR_ERROR)
		return;

	strcat(path, "/*.*");
	if (os_glob(path, 0, &glob) != 0)
		return;

	strcpy(path, new_path);

	for (size_t i = 0; i < glob->gl_pathc; i++) {
		struct os_globent ent = glob->gl_pathv[i];
		char *file;

		if (ent.directory)
			continue;

		file = strrchr(ent.path, '/');
		if (!file++)
			continue;

		if (astrcmpi(file, "scenes.json") == 0)
			continue;

		strcpy(new_path, path);
		strcat(new_path, "/");
		strcat(new_path, file);
		os_rename(ent.path, new_path);
	}

	os_globfree(glob);
}

static void move_basic_to_scene_collections(void)
{
	char path[512];
	char new_path[512];

	if (GetConfigPath(path, 512, "vk-games/basic") <= 0)
		return;
	if (!os_file_exists(path))
		return;

	if (GetConfigPath(new_path, 512, "vk-games/basic/scenes") <= 0)
		return;
	if (os_file_exists(new_path))
		return;

	if (os_mkdir(new_path) == MKDIR_ERROR)
		return;

	strcat(path, "/scenes.json");
	strcat(new_path, "/");
	strcat(new_path, Str("Untitled"));
	strcat(new_path, ".json");

	os_rename(path, new_path);
}

void OBSApp::AppInit()
{
	ProfileScope("OBSApp::AppInit");

	if (!InitApplicationBundle())
		throw "Failed to initialize application bundle";
	if (!MakeUserDirs())
		throw "Failed to create required user directories";
	if (!InitGlobalConfig())
		throw "Failed to initialize global config";
	if (!InitLocale())
		throw "Failed to load locale";
	if (!InitTheme())
		throw "Failed to load theme";

	config_set_default_string(globalConfig, "Basic", "Profile",
		Str("Untitled"));
	config_set_default_string(globalConfig, "Basic", "ProfileDir",
		Str("Untitled"));
	config_set_default_string(globalConfig, "Basic", "SceneCollection",
		Str("Untitled"));
	config_set_default_string(globalConfig, "Basic", "SceneCollectionFile",
		Str("Untitled"));

#ifdef __APPLE__
	if (config_get_bool(globalConfig, "Video", "DisableOSXVSync"))
		EnableOSXVSync(false);
#endif

	move_basic_to_profiles();
	move_basic_to_scene_collections();

	if (!MakeUserProfileDirs())
		throw "Failed to create profile directories";
}

const char *OBSApp::GetRenderModule() const
{
	const char *renderer = config_get_string(globalConfig, "Video",
		"Renderer");

	return (astrcmpi(renderer, "Direct3D 11") == 0) ?
		DL_D3D11 : DL_OPENGL;
}

static bool StartupOBS(const char *locale, profiler_name_store_t *store)
{
	char path[512];

	if (GetConfigPath(path, sizeof(path), "vk-games/plugin_config") <= 0)
		return false;

	return obs_startup(locale, path, store);
}

LoadingWinCloser *loadcloser = new LoadingWinCloser();
bool OBSApp::OBSInit()
{
	ProfileScope("OBSApp::OBSInit");

	bool licenseAccepted = config_get_bool(globalConfig, "General",
		"LicenseAccepted");
	OBSLicenseAgreement agreement(nullptr);
	const char *access_token = config_get_string(globalConfig,
		"Authentication", "Token");

	if (!licenseAccepted) {
		config_set_bool(globalConfig, "General",
			"LicenseAccepted", true);
		config_save(globalConfig);
	}
	bool guest = true;
	if (access_token && strlen(access_token)) {
		access_token_global = strdup(access_token);
		//guest = false;
	} else if (!enter_vk()) {
		return false;
	}
	QDialog *loadingwindow = new QDialog(NULL,
		Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
	loadingwindow->resize(192, 32);
	QFormLayout *form = new QFormLayout(loadingwindow);
	loadingwindow->setWindowTitle(QTStr("Vk.Title"));
	QLabel *loading = new QLabel(QTStr("Vk.Loading"));
	loading->setAlignment(Qt::AlignHCenter);
	form->addRow(loading);
	QObject::connect(loadcloser, SIGNAL(mainwindow_ready()),
		loadingwindow, SLOT(close()));
	loadingwindow->setAttribute(Qt::WA_DeleteOnClose, true);
	loadingwindow->show();
	QCoreApplication::processEvents();

	if (!StartupOBS(locale.c_str(), GetProfilerNameStore()))
		return false;

	mainWindow = new OBSBasic();

	mainWindow->setAttribute(Qt::WA_DeleteOnClose, true);
	connect(mainWindow, SIGNAL(destroyed()), this, SLOT(quit()));

	mainWindow->OBSInit();
	delete(loadcloser);
	/*if (guest) {
		QMessageBox greetings(mainWindow);
		greetings.setWindowTitle(QTStr("Vk.Title"));
		greetings.setText(QTStr("Vk.Greetings"));
		greetings.exec();
	}*/
	connect(this, &QGuiApplication::applicationStateChanged,
		[](Qt::ApplicationState state)
	{
		obs_hotkey_enable_background_press(
			state != Qt::ApplicationActive);
	});
	obs_hotkey_enable_background_press(
		applicationState() != Qt::ApplicationActive);
	return true;
}

string OBSApp::GetVersionString() const
{
	stringstream ver;

#ifdef HAVE_OBSCONFIG_H
	ver << OBS_VERSION;
#else
	ver << LIBOBS_API_MAJOR_VER << "." <<
		LIBOBS_API_MINOR_VER << "." <<
		LIBOBS_API_PATCH_VER;

#endif
	ver << " (";

#ifdef _WIN32
	if (sizeof(void*) == 8)
		ver << "64bit, ";

	ver << "windows)";
#elif __APPLE__
	ver << "mac)";
#elif __FreeBSD__
	ver << "freebsd)";
#else /* assume linux for the time being */
	ver << "linux)";
#endif

	return ver.str();
}

#ifdef __APPLE__
#define INPUT_AUDIO_SOURCE  "coreaudio_input_capture"
#define OUTPUT_AUDIO_SOURCE "coreaudio_output_capture"
#elif _WIN32
#define INPUT_AUDIO_SOURCE  "wasapi_input_capture"
#define OUTPUT_AUDIO_SOURCE "wasapi_output_capture"
#else
#define INPUT_AUDIO_SOURCE  "pulse_input_capture"
#define OUTPUT_AUDIO_SOURCE "pulse_output_capture"
#endif

const char *OBSApp::InputAudioSource() const
{
	return INPUT_AUDIO_SOURCE;
}

const char *OBSApp::OutputAudioSource() const
{
	return OUTPUT_AUDIO_SOURCE;
}

const char *OBSApp::GetLastLog() const
{
	return lastLogFile.c_str();
}

const char *OBSApp::GetCurrentLog() const
{
	return currentLogFile.c_str();
}

QString OBSTranslator::translate(const char *context, const char *sourceText,
	const char *disambiguation, int n) const
{
	const char *out = nullptr;
	if (!text_lookup_getstr(App()->GetTextLookup(), sourceText, &out))
		return QString();

	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(disambiguation);
	UNUSED_PARAMETER(n);
	return QT_UTF8(out);
}

static bool get_token(lexer *lex, string &str, base_token_type type)
{
	base_token token;
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;
	if (token.type != type)
		return false;

	str.assign(token.text.array, token.text.len);
	return true;
}

static bool expect_token(lexer *lex, const char *str, base_token_type type)
{
	base_token token;
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE))
		return false;
	if (token.type != type)
		return false;

	return strref_cmp(&token.text, str) == 0;
}

static uint64_t convert_log_name(const char *name)
{
	BaseLexer  lex;
	string     year, month, day, hour, minute, second;

	lexer_start(lex, name);

	if (!get_token(lex, year, BASETOKEN_DIGIT)) return 0;
	if (!expect_token(lex, "-", BASETOKEN_OTHER)) return 0;
	if (!get_token(lex, month, BASETOKEN_DIGIT)) return 0;
	if (!expect_token(lex, "-", BASETOKEN_OTHER)) return 0;
	if (!get_token(lex, day, BASETOKEN_DIGIT)) return 0;
	if (!get_token(lex, hour, BASETOKEN_DIGIT)) return 0;
	if (!expect_token(lex, "-", BASETOKEN_OTHER)) return 0;
	if (!get_token(lex, minute, BASETOKEN_DIGIT)) return 0;
	if (!expect_token(lex, "-", BASETOKEN_OTHER)) return 0;
	if (!get_token(lex, second, BASETOKEN_DIGIT)) return 0;

	stringstream timestring;
	timestring << year << month << day << hour << minute << second;
	return std::stoull(timestring.str());
}

static void delete_oldest_file(const char *location)
{
	BPtr<char>       logDir(GetConfigPathPtr(location));
	string           oldestLog;
	uint64_t         oldest_ts = (uint64_t)-1;
	struct os_dirent *entry;

	unsigned int maxLogs = (unsigned int)config_get_uint(
		App()->GlobalConfig(), "General", "MaxLogs");

	os_dir_t *dir = os_opendir(logDir);
	if (dir) {
		unsigned int count = 0;

		while ((entry = os_readdir(dir)) != NULL) {
			if (entry->directory || *entry->d_name == '.')
				continue;

			uint64_t ts = convert_log_name(entry->d_name);

			if (ts) {
				if (ts < oldest_ts) {
					oldestLog = entry->d_name;
					oldest_ts = ts;
				}

				count++;
			}
		}

		os_closedir(dir);

		if (count > maxLogs) {
			stringstream delPath;

			delPath << logDir << "/" << oldestLog;
			os_unlink(delPath.str().c_str());
		}
	}
}

static void get_last_log(void)
{
	BPtr<char>       logDir(GetConfigPathPtr("vk-games/logs"));
	struct os_dirent *entry;
	os_dir_t         *dir = os_opendir(logDir);
	uint64_t         highest_ts = 0;

	if (dir) {
		while ((entry = os_readdir(dir)) != NULL) {
			if (entry->directory || *entry->d_name == '.')
				continue;

			uint64_t ts = convert_log_name(entry->d_name);

			if (ts > highest_ts) {
				lastLogFile = entry->d_name;
				highest_ts = ts;
			}
		}

		os_closedir(dir);
	}
}

string GenerateTimeDateFilename(const char *extension, bool noSpace)
{
	time_t    now = time(0);
	char      file[256] = {};
	struct tm *cur_time;

	cur_time = localtime(&now);
	snprintf(file, sizeof(file), "%d-%02d-%02d%c%02d-%02d-%02d.%s",
		cur_time->tm_year + 1900,
		cur_time->tm_mon + 1,
		cur_time->tm_mday,
		noSpace ? '_' : ' ',
		cur_time->tm_hour,
		cur_time->tm_min,
		cur_time->tm_sec,
		extension);

	return string(file);
}

string GenerateSpecifiedFilename(const char *extension, bool noSpace,
	const char *format)
{
	time_t now = time(0);
	struct tm *cur_time;
	cur_time = localtime(&now);

	const size_t spec_count = 23;
	const char *spec[][2] = {
		{ "%CCYY", "%Y" },
		{ "%YY",   "%y" },
		{ "%MM",   "%m" },
		{ "%DD",   "%d" },
		{ "%hh",   "%H" },
		{ "%mm",   "%M" },
		{ "%ss",   "%S" },
		{ "%%",    "%%" },

		{ "%a",    "" },
		{ "%A",    "" },
		{ "%b",    "" },
		{ "%B",    "" },
		{ "%d",    "" },
		{ "%H",    "" },
		{ "%I",    "" },
		{ "%m",    "" },
		{ "%M",    "" },
		{ "%p",    "" },
		{ "%S",    "" },
		{ "%y",    "" },
		{ "%Y",    "" },
		{ "%z",    "" },
		{ "%Z",    "" },
	};

	char convert[128] = {};
	string sf = format;
	string c;
	size_t pos = 0, len;

	while (pos < sf.length()) {
		len = 0;
		for (size_t i = 0; i < spec_count && len == 0; i++) {

			if (sf.find(spec[i][0], pos) == pos) {
				if (strlen(spec[i][1]))
					strftime(convert, sizeof(convert),
						spec[i][1], cur_time);
				else
					strftime(convert, sizeof(convert),
						spec[i][0], cur_time);

				len = strlen(spec[i][0]);

				c = convert;
				if (c.length() && c.find_first_not_of(' ') !=
					std::string::npos)
					sf.replace(pos, len, convert);
			}
		}

		if (len)
			pos += strlen(convert);
		else if (!len && sf.at(pos) == '%')
			sf.erase(pos, 1);
		else
			pos++;
	}

	if (noSpace)
		replace(sf.begin(), sf.end(), ' ', '_');

	sf += '.';
	sf += extension;

	return (sf.length() < 256) ? sf : sf.substr(0, 255);
}

vector<pair<string, string>> GetLocaleNames()
{
	string path;
	if (!GetDataFilePath("locale.ini", path))
		throw "Could not find locale.ini path";

	ConfigFile ini;
	if (ini.Open(path.c_str(), CONFIG_OPEN_EXISTING) != 0)
		throw "Could not open locale.ini";

	size_t sections = config_num_sections(ini);

	vector<pair<string, string>> names;
	names.reserve(sections);
	for (size_t i = 0; i < sections; i++) {
		const char *tag = config_get_section(ini, i);
		const char *name = config_get_string(ini, tag, "Name");
		names.emplace_back(tag, name);
	}

	return names;
}

static void create_log_file(fstream &logFile)
{
	stringstream dst;

	get_last_log();

	currentLogFile = GenerateTimeDateFilename("txt");
	dst << "vk-games/logs/" << currentLogFile.c_str();

	BPtr<char> path(GetConfigPathPtr(dst.str().c_str()));
	logFile.open(path,
		ios_base::in | ios_base::out | ios_base::trunc);

	if (logFile.is_open()) {
		delete_oldest_file("vk-games/logs");
		base_set_log_handler(do_log, &logFile);
	}
	else {
		blog(LOG_ERROR, "Failed to open log file");
	}
}

static auto ProfilerNameStoreRelease = [](profiler_name_store_t *store)
{
	profiler_name_store_free(store);
};

using ProfilerNameStore =
std::unique_ptr<profiler_name_store_t,
	decltype(ProfilerNameStoreRelease)>;

ProfilerNameStore CreateNameStore()
{
	return ProfilerNameStore{ profiler_name_store_create(),
		ProfilerNameStoreRelease };
}

static auto SnapshotRelease = [](profiler_snapshot_t *snap)
{
	profile_snapshot_free(snap);
};

using ProfilerSnapshot =
std::unique_ptr<profiler_snapshot_t, decltype(SnapshotRelease)>;

ProfilerSnapshot GetSnapshot()
{
	return ProfilerSnapshot{ profile_snapshot_create(), SnapshotRelease };
}

static void SaveProfilerData(const ProfilerSnapshot &snap)
{
	if (currentLogFile.empty())
		return;

	auto pos = currentLogFile.rfind('.');
	if (pos == currentLogFile.npos)
		return;

#define LITERAL_SIZE(x) x, (sizeof(x) - 1)
	ostringstream dst;
	dst.write(LITERAL_SIZE("vk-games/profiler_data/"));
	dst.write(currentLogFile.c_str(), pos);
	dst.write(LITERAL_SIZE(".csv.gz"));
#undef LITERAL_SIZE

	BPtr<char> path = GetConfigPathPtr(dst.str().c_str());
	if (!profiler_snapshot_dump_csv_gz(snap.get(), path))
		blog(LOG_WARNING, "Could not save profiler data to '%s'",
			static_cast<const char*>(path));
}

static auto ProfilerFree = [](void *)
{
	profiler_stop();

	auto snap = GetSnapshot();

	profiler_print(snap.get());
	profiler_print_time_between_calls(snap.get());

	SaveProfilerData(snap);

	profiler_free();
};

void vk_init_config()
{
	size_t size;
	char buf[BUFSIZ];
	char destpath[512];
	char sourcepath[512];
	char checkpath[512];

	GetConfigPath(checkpath, sizeof(checkpath),
		"vk-games/basic/scenes/launch.txt");
	if (os_file_exists(checkpath))
		return;
	FILE *file = os_fopen(checkpath, "wb");
	fclose(file);
	GetConfigPath(destpath, sizeof(destpath), "vk-games/basic/scenes");
	strcat(destpath, "/");
	strcat(destpath, Str("Untitled"));
	strcat(destpath, ".json");
	strcpy(sourcepath, "../vk-games/basic/scenes/Untitled.json");

	FILE *source = os_fopen(sourcepath, "rb");
	FILE *dest = os_fopen(destpath, "wb");

	while (size = fread(buf, 1, BUFSIZ, source))
		fwrite(buf, 1, size, dest);

	fclose(source);
	fclose(dest);

	GetConfigPath(destpath, sizeof(destpath), "vk-games/basic/profiles");
	strcat(destpath, "/");
	strcat(destpath, Str("Untitled"));
	os_mkdir(destpath);
	strcat(destpath, "/");
	strcat(destpath, "basic");
	strcat(destpath, ".ini");
	strcpy(sourcepath, "../vk-games/basic/profiles/Untitled/basic.ini");

	source = os_fopen(sourcepath, "rb");
	dest = os_fopen(destpath, "wb");

	while (size = fread(buf, 1, BUFSIZ, source))
		fwrite(buf, 1, size, dest);

	fclose(source);
	fclose(dest);

	GetConfigPath(destpath, sizeof(destpath), "vk-games/basic/profiles");
	strcat(destpath, "/");
	strcat(destpath, Str("Untitled"));
	strcat(destpath, "/");
	strcat(destpath, "streamEncoder");
	strcat(destpath, ".json");
	strcpy(sourcepath,
		"../vk-games/basic/profiles/Untitled/streamEncoder.json");

	source = os_fopen(sourcepath, "rb");
	dest = os_fopen(destpath, "wb");

	while (size = fread(buf, 1, BUFSIZ, source))
		fwrite(buf, 1, size, dest);

	fclose(source);
	fclose(dest);
}

static const char *run_program_init = "run_program_init";
static int run_program(fstream &logFile, int argc, char *argv[])
{
	int ret = -1;

	auto profilerNameStore = CreateNameStore();

	std::unique_ptr<void, decltype(ProfilerFree)>
		prof_release(static_cast<void*>(&ProfilerFree),
			ProfilerFree);

	profiler_start();
	profile_register_root(run_program_init, 0);

	auto PrintInitProfile = [&]()
	{
		auto snap = GetSnapshot();

		profiler_snapshot_filter_roots(snap.get(), [](void *data,
			const char *name, bool *remove)
		{
			*remove = (*static_cast<const char**>(data)) != name;
			return true;
		}, static_cast<void*>(&run_program_init));

		profiler_print(snap.get());
	};

	ScopeProfiler prof{ run_program_init };

	QCoreApplication::addLibraryPath(".");

	OBSApp program(argc, argv, profilerNameStore.get());
	try {
		program.AppInit();

		OBSTranslator translator;

		create_log_file(logFile);
		delete_oldest_file("vk-games/profiler_data");

		program.installTranslator(&translator);
		vk_init_config();
		if (!program.OBSInit())
			return 0;

		prof.Stop();
		return program.exec();

	}
	catch (const char *error) {
		blog(LOG_ERROR, "%s", error);
		OBSErrorBox(nullptr, "%s", error);
	}

	return ret;
}

#define MAX_CRASH_REPORT_SIZE (50 * 1024)

#ifdef _WIN32

#define CRASH_MESSAGE \
	"Woops, OBS has crashed!\n\nWould you like to copy the crash log " \
	"to the clipboard?  (Crash logs will still be saved to the " \
	"%appdata%\\obs-studio\\crashes directory)"

static void main_crash_handler(const char *format, va_list args, void *param)
{
	char *text = new char[MAX_CRASH_REPORT_SIZE];

	vsnprintf(text, MAX_CRASH_REPORT_SIZE, format, args);
	text[MAX_CRASH_REPORT_SIZE - 1] = 0;

	delete_oldest_file("vk-games/crashes");

	string name = "vk-games/crashes/Crash ";
	name += GenerateTimeDateFilename("txt");

	BPtr<char> path(GetConfigPathPtr(name.c_str()));

	fstream file;
	file.open(path, ios_base::in | ios_base::out | ios_base::trunc |
		ios_base::binary);
	file << text;
	file.close();

	int ret = MessageBoxA(NULL, CRASH_MESSAGE, "OBS has crashed!",
		MB_YESNO | MB_ICONERROR | MB_TASKMODAL);

	if (ret == IDYES) {
		size_t len = strlen(text);

		HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
		memcpy(GlobalLock(mem), text, len);
		GlobalUnlock(mem);

		OpenClipboard(0);
		EmptyClipboard();
		SetClipboardData(CF_TEXT, mem);
		CloseClipboard();
	}

	exit(-1);

	UNUSED_PARAMETER(param);
}

static void load_debug_privilege(void)
{
	const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
	TOKEN_PRIVILEGES tp;
	HANDLE token;
	LUID val;

	if (!OpenProcessToken(GetCurrentProcess(), flags, &token)) {
		return;
	}

	if (!!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &val)) {
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Luid = val;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		AdjustTokenPrivileges(token, false, &tp,
			sizeof(tp), NULL, NULL);
	}

	CloseHandle(token);
}
#endif

#ifdef __APPLE__
#define BASE_PATH ".."
#else
#define BASE_PATH "../.."
#endif

#define CONFIG_PATH BASE_PATH "/config"

#ifndef OBS_UNIX_STRUCTURE
#define OBS_UNIX_STRUCTURE 0
#endif

int GetConfigPath(char *path, size_t size, const char *name)
{
	if (!OBS_UNIX_STRUCTURE && portable_mode) {
		if (name && *name) {
			return snprintf(path, size, CONFIG_PATH "/%s", name);
		}
		else {
			return snprintf(path, size, CONFIG_PATH);
		}
	}
	else {
		return os_get_config_path(path, size, name);
	}
}

char *GetConfigPathPtr(const char *name)
{
	if (!OBS_UNIX_STRUCTURE && portable_mode) {
		char path[512];

		if (snprintf(path, sizeof(path), CONFIG_PATH "/%s", name) > 0) {
			return bstrdup(path);
		}
		else {
			return NULL;
		}
	}
	else {
		return os_get_config_path_ptr(name);
	}
}

int GetProgramDataPath(char *path, size_t size, const char *name)
{
	return os_get_program_data_path(path, size, name);
}

char *GetProgramDataPathPtr(const char *name)
{
	return os_get_program_data_path_ptr(name);
}

bool GetFileSafeName(const char *name, std::string &file)
{
	size_t base_len = strlen(name);
	size_t len = os_utf8_to_wcs(name, base_len, nullptr, 0);
	std::wstring wfile;

	if (!len)
		return false;

	wfile.resize(len);
	os_utf8_to_wcs(name, base_len, &wfile[0], len);

	for (size_t i = wfile.size(); i > 0; i--) {
		size_t im1 = i - 1;

		if (iswspace(wfile[im1])) {
			wfile[im1] = '_';
		}
		else if (wfile[im1] != '_' && !iswalnum(wfile[im1])) {
			wfile.erase(im1, 1);
		}
	}

	if (wfile.size() == 0)
		wfile = L"characters_only";

	len = os_wcs_to_utf8(wfile.c_str(), wfile.size(), nullptr, 0);
	if (!len)
		return false;

	file.resize(len);
	os_wcs_to_utf8(wfile.c_str(), wfile.size(), &file[0], len);
	return true;
}

bool GetClosestUnusedFileName(std::string &path, const char *extension)
{
	size_t len = path.size();
	if (extension) {
		path += ".";
		path += extension;
	}

	if (!os_file_exists(path.c_str()))
		return true;

	int index = 1;

	do {
		path.resize(len);
		path += std::to_string(++index);
		if (extension) {
			path += ".";
			path += extension;
		}
	} while (os_file_exists(path.c_str()));

	return true;
}

bool WindowPositionValid(int x, int y)
{
	vector<MonitorInfo> monitors;
	GetMonitors(monitors);

	for (auto &monitor : monitors) {
		int br_x = monitor.x + monitor.cx;
		int br_y = monitor.y + monitor.cy;

		if (x >= monitor.x && x < br_x &&
			y >= monitor.y && y < br_y)
			return true;
	}

	return false;
}

static inline bool arg_is(const char *arg,
	const char *long_form, const char *short_form)
{
	return (long_form  && strcmp(arg, long_form) == 0) ||
		(short_form && strcmp(arg, short_form) == 0);
}

#if !defined(_WIN32) && !defined(__APPLE__)
#define IS_UNIX 1
#endif

/* if using XDG and was previously using an older build of OBS, move config
* files to XDG directory */
#if defined(USE_XDG) && defined(IS_UNIX)
static void move_to_xdg(void)
{
	char old_path[512];
	char new_path[512];
	char *home = getenv("HOME");
	if (!home)
		return;

	if (snprintf(old_path, 512, "%s/.obs-studio", home) <= 0)
		return;

	/* make base xdg path if it doesn't already exist */
	if (GetConfigPath(new_path, 512, "") <= 0)
		return;
	if (os_mkdirs(new_path) == MKDIR_ERROR)
		return;

	if (GetConfigPath(new_path, 512, "obs-studio") <= 0)
		return;

	if (os_file_exists(old_path) && !os_file_exists(new_path)) {
		rename(old_path, new_path);
	}
}
#endif

static bool update_ffmpeg_output(ConfigFile &config)
{
	if (config_has_user_value(config, "AdvOut", "FFOutputToFile"))
		return false;

	const char *url = config_get_string(config, "AdvOut", "FFURL");
	if (!url)
		return false;

	bool isActualURL = strstr(url, "://") != nullptr;
	if (isActualURL)
		return false;

	string urlStr = url;
	string extension;

	for (size_t i = urlStr.length(); i > 0; i--) {
		size_t idx = i - 1;

		if (urlStr[idx] == '.') {
			extension = &urlStr[i];
		}

		if (urlStr[idx] == '\\' || urlStr[idx] == '/') {
			urlStr[idx] = 0;
			break;
		}
	}

	if (urlStr.empty() || extension.empty())
		return false;

	config_remove_value(config, "AdvOut", "FFURL");
	config_set_string(config, "AdvOut", "FFFilePath", urlStr.c_str());
	config_set_string(config, "AdvOut", "FFExtension", extension.c_str());
	config_set_bool(config, "AdvOut", "FFOutputToFile", true);
	return true;
}

static bool move_reconnect_settings(ConfigFile &config, const char *sec)
{
	bool changed = false;

	if (config_has_user_value(config, sec, "Reconnect")) {
		bool reconnect = config_get_bool(config, sec, "Reconnect");
		config_set_bool(config, "Output", "Reconnect", reconnect);
		changed = true;
	}
	if (config_has_user_value(config, sec, "RetryDelay")) {
		int delay = (int)config_get_uint(config, sec, "RetryDelay");
		config_set_uint(config, "Output", "RetryDelay", delay);
		changed = true;
	}
	if (config_has_user_value(config, sec, "MaxRetries")) {
		int retries = (int)config_get_uint(config, sec, "MaxRetries");
		config_set_uint(config, "Output", "MaxRetries", retries);
		changed = true;
	}

	return changed;
}

static bool update_reconnect(ConfigFile &config)
{
	if (!config_has_user_value(config, "Output", "Mode"))
		return false;

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode)
		return false;

	const char *section = (strcmp(mode, "Advanced") == 0) ?
		"AdvOut" : "SimpleOutput";

	if (move_reconnect_settings(config, section)) {
		config_remove_value(config, "SimpleOutput", "Reconnect");
		config_remove_value(config, "SimpleOutput", "RetryDelay");
		config_remove_value(config, "SimpleOutput", "MaxRetries");
		config_remove_value(config, "AdvOut", "Reconnect");
		config_remove_value(config, "AdvOut", "RetryDelay");
		config_remove_value(config, "AdvOut", "MaxRetries");
		return true;
	}

	return false;
}

static void convert_x264_settings(obs_data_t *data)
{
	bool use_bufsize = obs_data_get_bool(data, "use_bufsize");

	if (use_bufsize) {
		int buffer_size = (int)obs_data_get_int(data, "buffer_size");
		if (buffer_size == 0)
			obs_data_set_string(data, "rate_control", "CRF");
	}
}

static void convert_14_2_encoder_setting(const char *encoder, const char *file)
{
	obs_data_t *data = obs_data_create_from_json_file_safe(file, "bak");
	obs_data_item_t *cbr_item = obs_data_item_byname(data, "cbr");
	obs_data_item_t *rc_item = obs_data_item_byname(data, "rate_control");
	bool modified = false;
	bool cbr = true;

	if (cbr_item) {
		cbr = obs_data_item_get_bool(cbr_item);
		obs_data_item_unset_user_value(cbr_item);

		obs_data_set_string(data, "rate_control", cbr ? "CBR" : "VBR");

		modified = true;
	}

	if (!rc_item && astrcmpi(encoder, "obs_x264") == 0) {
		if (!cbr_item)
			obs_data_set_string(data, "rate_control", "CBR");
		else if (!cbr)
			convert_x264_settings(data);

		modified = true;
	}

	if (modified)
		obs_data_save_json_safe(data, file, "tmp", "bak");

	obs_data_item_release(&rc_item);
	obs_data_item_release(&cbr_item);
	obs_data_release(data);
}

static void upgrade_settings(void)
{
	char path[512];
	int pathlen = GetConfigPath(path, 512, "vk-games/basic/profiles");

	if (pathlen <= 0)
		return;
	if (!os_file_exists(path))
		return;

	os_dir_t *dir = os_opendir(path);
	if (!dir)
		return;

	struct os_dirent *ent = os_readdir(dir);

	while (ent) {
		if (ent->directory && strcmp(ent->d_name, ".") != 0 &&
			strcmp(ent->d_name, "..") != 0) {
			strcat(path, "/");
			strcat(path, ent->d_name);
			strcat(path, "/basic.ini");

			ConfigFile config;
			int ret;

			ret = config.Open(path, CONFIG_OPEN_EXISTING);
			if (ret == CONFIG_SUCCESS) {
				if (update_ffmpeg_output(config) ||
					update_reconnect(config)) {
					config_save_safe(config, "tmp",
						nullptr);
				}
			}


			if (config) {
				const char *sEnc = config_get_string(config,
					"AdvOut", "Encoder");
				const char *rEnc = config_get_string(config,
					"AdvOut", "RecEncoder");

				/* replace "cbr" option with "rate_control" for
				* each profile's encoder data */
				path[pathlen] = 0;
				strcat(path, "/");
				strcat(path, ent->d_name);
				strcat(path, "/recordEncoder.json");
				convert_14_2_encoder_setting(rEnc, path);

				path[pathlen] = 0;
				strcat(path, "/");
				strcat(path, ent->d_name);
				strcat(path, "/streamEncoder.json");
				convert_14_2_encoder_setting(sEnc, path);
			}

			path[pathlen] = 0;
		}

		ent = os_readdir(dir);
	}

	os_closedir(dir);
}

int main(int argc, char *argv[])
{
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
	load_debug_privilege();
	base_set_crash_handler(main_crash_handler, nullptr);
#endif

	base_get_log_handler(&def_log_handler, nullptr);

#if defined(USE_XDG) && defined(IS_UNIX)
	move_to_xdg();
#endif

	for (int i = 1; i < argc; i++) {
		if (arg_is(argv[i], "--portable", "-p")) {
			portable_mode = true;

		}
		else if (arg_is(argv[i], "--startstreaming", nullptr)) {
			opt_start_streaming = true;

		}
		else if (arg_is(argv[i], "--startrecording", nullptr)) {
			opt_start_recording = true;

		}
		else if (arg_is(argv[i], "--collection", nullptr)) {
			if (++i < argc) opt_starting_collection = argv[i];

		}
		else if (arg_is(argv[i], "--profile", nullptr)) {
			if (++i < argc) opt_starting_profile = argv[i];

		}
		else if (arg_is(argv[i], "--scene", nullptr)) {
			if (++i < argc) opt_starting_scene = argv[i];
		}
	}

#if !OBS_UNIX_STRUCTURE
	if (!portable_mode) {
		portable_mode =
			os_file_exists(BASE_PATH "/portable_mode") ||
			os_file_exists(BASE_PATH "/obs_portable_mode") ||
			os_file_exists(BASE_PATH "/portable_mode.txt") ||
			os_file_exists(BASE_PATH "/obs_portable_mode.txt");
	}
#endif

	upgrade_settings();

	fstream logFile;

	curl_global_init(CURL_GLOBAL_ALL);
	int ret = run_program(logFile, argc, argv);

	free(access_token_global);
	blog(LOG_INFO, "Number of memory leaks: %ld", bnum_allocs());
	base_set_log_handler(nullptr, nullptr);
	return ret;
}