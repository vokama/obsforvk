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

#pragma once

#include <QApplication>
#include <QTranslator>
#include <QPointer>
#include <obs.hpp>
#include <util/lexer.h>
#include <util/profiler.h>
#include <util/util.hpp>
#include <util/platform.h>
#include <string>
#include <memory>
#include <vector>

#include "window-main.hpp"

std::string CurrentTimeString();
std::string CurrentDateTimeString();
std::string GenerateTimeDateFilename(const char *extension, bool noSpace=false);
std::string GenerateSpecifiedFilename(const char *extension, bool noSpace,
					const char *format);
QObject *CreateShortcutFilter();

struct BaseLexer {
	lexer lex;
public:
	inline BaseLexer() {lexer_init(&lex);}
	inline ~BaseLexer() {lexer_free(&lex);}
	operator lexer*() {return &lex;}
};

class LoginWinHandler : public QObject
{
	Q_OBJECT

public:
	LoginWinHandler() {}

	QLineEdit *usernameField;
	QLineEdit *passwordField;
	QDialog *loginWin;
	int errorcode;
	public Q_SLOTS :
		void process_login();
	Q_SIGNALS:
		void token_received();
};

class CodeWinHandler : public QObject
{
	Q_OBJECT

public:
	CodeWinHandler() {}

	void *button_data;
	QLineEdit *input;
	public Q_SLOTS :
		void check_input(const QString & text);
};

class LoadingWinCloser : public QObject
{
	Q_OBJECT

public:
	LoadingWinCloser() {}

	Q_SIGNALS:
		 void mainwindow_ready();
};

extern LoadingWinCloser *loadcloser;


class OBSTranslator : public QTranslator {
	Q_OBJECT

public:
	virtual bool isEmpty() const override {return false;}

	virtual QString translate(const char *context, const char *sourceText,
			const char *disambiguation, int n) const override;
};

class OBSApp : public QApplication {
	Q_OBJECT

private:
	std::string                    locale;
	std::string		       theme;
	ConfigFile                     globalConfig;
	TextLookup                     textLookup;
	OBSContext                     obsContext;
	QPointer<OBSMainWindow>        mainWindow;
	profiler_name_store_t          *profilerNameStore = nullptr;

	os_inhibit_t                   *sleepInhibitor = nullptr;
	int                            sleepInhibitRefs = 0;

	bool InitGlobalConfig();
	bool InitGlobalConfigDefaults();
	bool InitLocale();
	bool InitTheme();

public:
	OBSApp(int &argc, char **argv, profiler_name_store_t *store);
	~OBSApp();

	void AppInit();
	bool OBSInit();

	inline QMainWindow *GetMainWindow() const {return mainWindow.data();}

	inline config_t *GlobalConfig() const {return globalConfig;}

	inline const char *GetLocale() const
	{
		return locale.c_str();
	}

	inline const char *GetTheme() const {return theme.c_str();}
	bool SetTheme(std::string name, std::string path = "");

	inline lookup_t *GetTextLookup() const {return textLookup;}

	inline const char *GetString(const char *lookupVal) const
	{
		return textLookup.GetString(lookupVal);
	}

	profiler_name_store_t *GetProfilerNameStore() const
	{
		return profilerNameStore;
	}

	const char *GetLastLog() const;
	const char *GetCurrentLog() const;

	std::string GetVersionString() const;

	const char *InputAudioSource() const;
	const char *OutputAudioSource() const;

	const char *GetRenderModule() const;

	inline void IncrementSleepInhibition()
	{
		if (!sleepInhibitor) return;
		if (sleepInhibitRefs++ == 0)
			os_inhibit_sleep_set_active(sleepInhibitor, true);
	}

	inline void DecrementSleepInhibition()
	{
		if (!sleepInhibitor) return;
		if (sleepInhibitRefs == 0) return;
		if (--sleepInhibitRefs == 0)
			os_inhibit_sleep_set_active(sleepInhibitor, false);
	}
};

int GetConfigPath(char *path, size_t size, const char *name);
char *GetConfigPathPtr(const char *name);

int GetProgramDataPath(char *path, size_t size, const char *name);
char *GetProgramDataPathPtr(const char *name);

inline OBSApp *App() {return static_cast<OBSApp*>(qApp);}

inline config_t *GetGlobalConfig() {return App()->GlobalConfig();}

std::vector<std::pair<std::string, std::string>> GetLocaleNames();
inline const char *Str(const char *lookup) {return App()->GetString(lookup);}
#define QTStr(lookupVal) QString::fromUtf8(Str(lookupVal))

bool GetFileSafeName(const char *name, std::string &file);
bool GetClosestUnusedFileName(std::string &path, const char *extension);

bool WindowPositionValid(int x, int y);

static inline int GetProfilePath(char *path, size_t size, const char *file)
{
	OBSMainWindow *window = reinterpret_cast<OBSMainWindow*>(
			App()->GetMainWindow());
	return window->GetProfilePath(path, size, file);
}

extern char *access_token_global;
extern bool opt_start_streaming;
extern bool opt_start_recording;
extern std::string opt_starting_scene;
