/*
	Copyright 2023 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

    Hollycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Hollycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include "jni_util.h"
#include <cerrno>
#include <jni.h>
#include <unistd.h>

namespace hostfs
{

class AndroidStorage : public CustomStorage
{
public:
	void init(JNIEnv *env, jobject storage)
	{
		jstorage = env->NewGlobalRef(storage);
		jni::Class clazz(env->GetObjectClass(storage));
		jopenFile = env->GetMethodID(clazz, "openFile", "(Ljava/lang/String;Ljava/lang/String;)I");
		jlistContent = env->GetMethodID(clazz, "listContent", "(Ljava/lang/String;)[Lcom/hollycast/emulator/FileInfo;");
		jgetParentUri = env->GetMethodID(clazz, "getParentUri", "(Ljava/lang/String;)Ljava/lang/String;");
		jgetSubPath = env->GetMethodID(clazz, "getSubPath", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
		jgetFileInfo = env->GetMethodID(clazz, "getFileInfo", "(Ljava/lang/String;)Lcom/hollycast/emulator/FileInfo;");
		jexists = env->GetMethodID(clazz, "exists", "(Ljava/lang/String;)Z");
		jdeleteDocument = env->GetMethodID(clazz, "deleteDocument", "(Ljava/lang/String;)Z");
		jrenameDocument = env->GetMethodID(clazz, "renameDocument", "(Ljava/lang/String;Ljava/lang/String;)Z");
		jaddStorage = env->GetMethodID(clazz, "addStorage", "(ZZLjava/lang/String;Ljava/lang/String;)Z");
		jsaveScreenshot = env->GetMethodID(clazz, "saveScreenshot", "(Ljava/lang/String;[B)V");
		jimportHomeDirectory = env->GetMethodID(clazz, "importHomeDirectory", "()V");
		jexportHomeDirectory = env->GetMethodID(clazz, "exportHomeDirectory", "()V");
		jrequiresSafFilePicker = env->GetMethodID(clazz, "requiresSafFilePicker", "()Z");
	}

	bool isKnownPath(const std::string& path) override {
		return path.substr(0, 10) == "content://";
	}

	File *openFile(const std::string& uri, const std::string& mode) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage openFile begin: uri='%s' mode='%s'", uri.c_str(), mode.c_str());
		jni::String juri(uri);
		const char *amode;
		if (mode.substr(0, 2) == "r+")
			amode = "rw";
		else if (mode[0] == 'r')
			amode = "r";
		else if (mode.substr(0, 2) == "w+")
			amode = "rwt";
		else if (mode[0] == 'w')
			amode = "wt";
		else if (mode.substr(0, 2) == "a+")
			// Incorrect but this is a weird mode anyway
			amode = "rw";
		else // a
			amode = "wa";
	    jni::String jmode(amode);
		jint fd = jni::env()->CallIntMethod(jstorage, jopenFile, (jstring)juri, (jstring)jmode);
		try {
			checkException();
		} catch (const hostfs::StorageException& e) {
			WARN_LOG(COMMON, "openFile failed: %s", e.what());
			return nullptr;
		}
		if (fd < 0)
		{
			WARN_LOG(COMMON, "openFile failed: invalid fd for uri='%s' mode='%s'", uri.c_str(), mode.c_str());
			return nullptr;
		}
		FILE *file = fdopen(fd, mode.c_str());
		if (!file)
		{
			WARN_LOG(COMMON, "openFile failed: fdopen failed for uri='%s' mode='%s' errno=%d", uri.c_str(), mode.c_str(), errno);
			::close(fd);
			return nullptr;
		}
		NOTICE_LOG(COMMON, "AndroidStorage openFile success: uri='%s' mode='%s' fd=%d", uri.c_str(), mode.c_str(), fd);
		return new StdFile(file);
	}

	std::vector<FileInfo> listContent(const std::string& uri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage listContent begin: uri='%s'", uri.c_str());
		std::vector<FileInfo> ret;
		if (uri.empty())
			// Nothing to see here
			return ret;

		JNIEnv *env = jni::env();
		jni::String juri(uri);
		jni::ObjectArray<> fileInfos(env->CallObjectMethod(jstorage, jlistContent, (jstring)juri));
		checkException();
		int len = fileInfos.size();
		for (int i = 0; i < len; i++)
		{
			jni::Object fileInfo = fileInfos[i];
			ret.emplace_back(fromJavaFileInfo(fileInfo));
		}
		NOTICE_LOG(COMMON, "AndroidStorage listContent success: uri='%s' entries=%d", uri.c_str(), len);

		return ret;
	}

	std::string getParentPath(const std::string& uri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage getParentPath begin: uri='%s'", uri.c_str());
		jni::String juri(uri);
		jni::String jparentUri(jni::env()->CallObjectMethod(jstorage, jgetParentUri, (jstring)juri));
		checkException();
		std::string parent = jparentUri;
		NOTICE_LOG(COMMON, "AndroidStorage getParentPath success: uri='%s' parent='%s'", uri.c_str(), parent.c_str());
		return parent;
	}

	std::string getSubPath(const std::string& reference, const std::string& relative) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage getSubPath begin: reference='%s' relative='%s'", reference.c_str(), relative.c_str());
		jni::String jref(reference);
		jni::String jrel(relative);
		jni::String jretUri(jni::env()->CallObjectMethod(jstorage, jgetSubPath, (jstring)jref, (jstring)jrel));
		checkException();
		std::string result = jretUri;
		NOTICE_LOG(COMMON, "AndroidStorage getSubPath success: reference='%s' relative='%s' result='%s'",
			reference.c_str(), relative.c_str(), result.c_str());
		return result;
	}

	FileInfo getFileInfo(const std::string& uri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage getFileInfo begin: uri='%s'", uri.c_str());
		jni::String juri(uri);
		jni::Object jinfo(jni::env()->CallObjectMethod(jstorage, jgetFileInfo, (jstring)juri));
		checkException();
		FileInfo info = fromJavaFileInfo(jinfo);
		NOTICE_LOG(COMMON, "AndroidStorage getFileInfo success: uri='%s' name='%s' isDirectory=%d size=%llu writable=%d",
			uri.c_str(), info.name.c_str(), info.isDirectory ? 1 : 0, (unsigned long long)info.size, info.isWritable ? 1 : 0);
		return info;
	}

	bool exists(const std::string& uri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage exists begin: uri='%s'", uri.c_str());
		jni::String juri(uri);
		bool ret = jni::env()->CallBooleanMethod(jstorage, jexists, (jstring)juri);
		try {
			checkException();
			NOTICE_LOG(COMMON, "AndroidStorage exists success: uri='%s' exists=%d", uri.c_str(), ret ? 1 : 0);
			return ret;
		} catch (...) {
			WARN_LOG(COMMON, "AndroidStorage exists failed: uri='%s'", uri.c_str());
			return false;
		}
	}

	int removeFile(const std::string& uri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage delete begin: uri='%s'", uri.c_str());
		jni::String juri(uri);
		bool ret = jni::env()->CallBooleanMethod(jstorage, jdeleteDocument, (jstring)juri);
		try {
			checkException();
			NOTICE_LOG(COMMON, "AndroidStorage delete finished: uri='%s' success=%d", uri.c_str(), ret ? 1 : 0);
		} catch (...) {
			WARN_LOG(COMMON, "AndroidStorage delete failed: uri='%s'", uri.c_str());
			ret = false;
		}
		if (!ret)
			errno = EACCES;
		return ret ? 0 : -1;
	}

	int renameFile(const std::string& oldUri, const std::string& newUri) override
	{
		NOTICE_LOG(COMMON, "AndroidStorage rename begin: old='%s' new='%s'", oldUri.c_str(), newUri.c_str());
		jni::String joldUri(oldUri);
		jni::String jnewUri(newUri);
		bool ret = jni::env()->CallBooleanMethod(jstorage, jrenameDocument, (jstring)joldUri, (jstring)jnewUri);
		try {
			checkException();
			NOTICE_LOG(COMMON, "AndroidStorage rename finished: old='%s' new='%s' success=%d",
				oldUri.c_str(), newUri.c_str(), ret ? 1 : 0);
		} catch (...) {
			WARN_LOG(COMMON, "AndroidStorage rename failed: old='%s' new='%s'", oldUri.c_str(), newUri.c_str());
			ret = false;
		}
		if (!ret)
			errno = EACCES;
		return ret ? 0 : -1;
	}

	bool addStorage(bool isDirectory, bool writeAccess, const std::string& description,
			void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType) override
	{
		if (!config::UseSafFilePicker && !jni::env()->CallBooleanMethod(jstorage, jrequiresSafFilePicker))
			return false;
		jni::String jdesc(description);
		jni::String jmimeType(mimeType);
		bool ret = jni::env()->CallBooleanMethod(jstorage, jaddStorage, isDirectory, writeAccess, (jstring)jdesc, (jstring)jmimeType);
		checkException();
		if (ret)
			addStorageCallback = callback;
		return ret;
	}

	void doStorageCallback(jstring path)
	{
		if (addStorageCallback != nullptr)
		{
			try {
				addStorageCallback(path == nullptr, jni::String(path, false));
			} catch (...) {
			}
			addStorageCallback = nullptr;
		}
	}

	void saveScreenshot(const std::string& name, const std::vector<u8>& data)
	{
		jni::String jname(name);
		jni::ByteArray jdata(data.size());
		jdata.setData(&data[0]);
		jni::env()->CallVoidMethod(jstorage, jsaveScreenshot, (jstring)jname, (jbyteArray)jdata);
		checkException();
	}

	void importHomeDirectory() {
		jni::env()->CallVoidMethod(jstorage, jimportHomeDirectory);
		checkException();
	}

	void exportHomeDirectory() {
		jni::env()->CallVoidMethod(jstorage, jexportHomeDirectory);
		checkException();
	}

private:
	void checkException()
	{
		jni::Throwable throwable(jni::env()->ExceptionOccurred());
		if (throwable.isNull())
			return;

		jni::env()->ExceptionClear();
		throw StorageException(strprintf(i18n::T("Storage access failed: %s"), throwable.getMessage().c_str()));
	}

	FileInfo fromJavaFileInfo(const jni::Object& jinfo)
	{
		loadFileInfoMethods(jinfo);

		FileInfo info;
		JNIEnv *env = jni::env();
		info.name = jni::String(env->CallObjectMethod(jinfo, jgetName));
		info.path = jni::String(env->CallObjectMethod(jinfo, jgetPath));
		info.isDirectory = env->CallBooleanMethod(jinfo, jisDirectory);
		info.isWritable = env->CallBooleanMethod(jinfo, jisWritable);
		info.size = env->CallLongMethod(jinfo, jgetSize);
		info.updateTime = env->CallLongMethod(jinfo, jgetUpdateTime);

		return info;
	}

	void loadFileInfoMethods(const jni::Object& jinfo)
	{
		if (jgetName != nullptr)
			return;
		JNIEnv *env = jni::env();
		jni::Class infoClass = jinfo.getClass();
		checkException();
		jgetName = env->GetMethodID(infoClass, "getName", "()Ljava/lang/String;");
		jgetPath = env->GetMethodID(infoClass, "getPath", "()Ljava/lang/String;");
		jisDirectory = env->GetMethodID(infoClass, "isDirectory", "()Z");
		jisWritable = env->GetMethodID(infoClass, "isWritable", "()Z");
		jgetSize = env->GetMethodID(infoClass, "getSize", "()J");
		jgetUpdateTime = env->GetMethodID(infoClass, "getUpdateTime", "()J");
	}

	jobject jstorage;
	jmethodID jopenFile;
	jmethodID jlistContent;
	jmethodID jgetParentUri;
	jmethodID jaddStorage;
	jmethodID jgetSubPath;
	jmethodID jgetFileInfo;
	jmethodID jexists;
	jmethodID jdeleteDocument;
	jmethodID jrenameDocument;
	jmethodID jsaveScreenshot;
	jmethodID jexportHomeDirectory;
	jmethodID jimportHomeDirectory;
	jmethodID jrequiresSafFilePicker;
	// FileInfo accessors lazily initialized to avoid having to load the class
	jmethodID jgetName = nullptr;
	jmethodID jgetPath = nullptr;
	jmethodID jisDirectory = nullptr;
	jmethodID jisWritable = nullptr;
	jmethodID jgetSize = nullptr;
	jmethodID jgetUpdateTime = nullptr;
	void (*addStorageCallback)(bool cancelled, std::string selectedPath);
};

CustomStorage& customStorage()
{
	static std::unique_ptr<AndroidStorage> androidStorage;
	if (!androidStorage)
		androidStorage = std::make_unique<AndroidStorage>();
	return *androidStorage;
}

void saveScreenshot(const std::string& name, const std::vector<u8>& data)
{
	return static_cast<AndroidStorage&>(customStorage()).saveScreenshot(name, data);
}

void importHomeDirectory() {
	static_cast<AndroidStorage&>(customStorage()).importHomeDirectory();
}

void exportHomeDirectory() {
	static_cast<AndroidStorage&>(customStorage()).exportHomeDirectory();
}

}	// namespace hostfs

extern "C" JNIEXPORT void JNICALL Java_com_hollycast_emulator_AndroidStorage_addStorageCallback(JNIEnv *env, jobject obj, jstring path)
{
	static_cast<hostfs::AndroidStorage&>(hostfs::customStorage()).doStorageCallback(path);
}

extern "C" JNIEXPORT void JNICALL Java_com_hollycast_emulator_AndroidStorage_init(JNIEnv *env, jobject jstorage)
{
	static_cast<hostfs::AndroidStorage&>(hostfs::customStorage()).init(env, jstorage);
}

extern "C" JNIEXPORT void JNICALL Java_com_hollycast_emulator_AndroidStorage_reloadConfig(JNIEnv *env)
{
	if (config::open())
	{
		const RenderType render = config::RendererType;
		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		// Make sure the renderer type doesn't change mid-flight
		config::RendererType = render;
		config::Settings::instance().save();
	}
}
