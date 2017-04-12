#include <string>
#include <algorithm>
#include <QMessageBox>
#include "audio-encoders.hpp"
#include "window-basic-main.hpp"
#include "window-basic-main-outputs.hpp"

using namespace std;

static void OBSStreamStarting(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler*>(data);
	obs_output_t *obj = (obs_output_t*)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		return;

	output->delayActive = true;
	QMetaObject::invokeMethod(output->main,
			"StreamDelayStarting", Q_ARG(int, sec));
}

static void OBSStreamStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler*>(data);
	obs_output_t *obj = (obs_output_t*)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		QMetaObject::invokeMethod(output->main, "StreamStopping");
	else
		QMetaObject::invokeMethod(output->main,
				"StreamDelayStopping", Q_ARG(int, sec));
}

static void OBSStartStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler*>(data);
	output->streamingActive = true;
	QMetaObject::invokeMethod(output->main, "StreamingStart");

	UNUSED_PARAMETER(params);
}

static void OBSStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler*>(data);
	int code = (int)calldata_int(params, "code");

	output->streamingActive = false;
	output->delayActive = false;
	QMetaObject::invokeMethod(output->main,
			"StreamingStop", Q_ARG(int, code));
}

static void OBSRecordStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler*>(data);
	QMetaObject::invokeMethod(output->main, "RecordStopping");

	UNUSED_PARAMETER(params);
}

static void FindBestFilename(string &strPath, bool noSpace)
{
	int num = 2;

	if (!os_file_exists(strPath.c_str()))
		return;

	const char *ext = strrchr(strPath.c_str(), '.');
	if (!ext)
		return;

	int extStart = int(ext - strPath.c_str());
	for (;;) {
		string testPath = strPath;
		string numStr;

		numStr = noSpace ? "_" : " (";
		numStr += to_string(num++);
		if (!noSpace)
			numStr += ")";

		testPath.insert(extStart, numStr);

		if (!os_file_exists(testPath.c_str())) {
			strPath = testPath;
			break;
		}
	}
}

/* ------------------------------------------------------------------------ */

static bool CreateAACEncoder(OBSEncoder &res, string &id, int bitrate,
		const char *name, size_t idx)
{
	const char *id_ = GetAACEncoderForBitrate(bitrate);
	if (!id_) {
		id.clear();
		res = nullptr;
		return false;
	}

	if (id == id_)
		return true;

	id = id_;
	res = obs_audio_encoder_create(id_, name, nullptr, idx, nullptr);

	if (res) {
		obs_encoder_release(res);
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------------ */

struct SimpleOutput : BasicOutputHandler {
	OBSEncoder             aacStreaming;
	OBSEncoder             h264Streaming;
	OBSEncoder             aacRecording;
	OBSEncoder             h264Recording;

	string                 aacRecEncID;
	string                 aacStreamEncID;

	string                 videoEncoder;
	string                 videoQuality;
	bool                   usingRecordingPreset = false;
	bool                   ffmpegOutput = false;
	bool                   lowCPUx264 = false;

	SimpleOutput(OBSBasic *main_);

	int CalcCRF(int crf);

	void UpdateRecordingSettings_x264_crf(int crf);
	void UpdateRecordingSettings_qsv11(int crf);
	void UpdateRecordingSettings_nvenc(int cqp);
	void UpdateRecordingSettings();
	void UpdateRecordingAudioSettings();
	virtual void Update() override;

	void SetupOutputs();
	int GetAudioBitrate() const;

	void LoadRecordingPreset_h264(const char *encoder);
	void LoadRecordingPreset_Lossless();
	void LoadRecordingPreset();

	void LoadStreamingPreset_h264(const char *encoder);

	virtual bool StartStreaming(obs_service_t *service) override;
	virtual bool StartRecording() override;
	virtual void StopStreaming() override;
	virtual void ForceStopStreaming() override;
	virtual void StopRecording() override;
	virtual bool StreamingActive() const override;
	virtual bool RecordingActive() const override;
};

void SimpleOutput::LoadRecordingPreset_Lossless()
{
	fileOutput = obs_output_create("ffmpeg_output",
			"simple_ffmpeg_output", nullptr, nullptr);
	if (!fileOutput)
		throw "Failed to create recording FFmpeg output "
		      "(simple output)";
	obs_output_release(fileOutput);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "format_name", "avi");
	obs_data_set_string(settings, "video_encoder", "utvideo");
	obs_data_set_string(settings, "audio_encoder", "pcm_s16le");

	obs_output_update(fileOutput, settings);
	obs_data_release(settings);
}

void SimpleOutput::LoadRecordingPreset_h264(const char *encoderId)
{
	h264Recording = obs_video_encoder_create(encoderId,
			"simple_h264_recording", nullptr, nullptr);
	if (!h264Recording)
		throw "Failed to create h264 recording encoder (simple output)";
	obs_encoder_release(h264Recording);
}

void SimpleOutput::LoadStreamingPreset_h264(const char *encoderId)
{
	h264Streaming = obs_video_encoder_create(encoderId,
			"simple_h264_stream", nullptr, nullptr);
	if (!h264Streaming)
		throw "Failed to create h264 streaming encoder (simple output)";
	obs_encoder_release(h264Streaming);
}

void SimpleOutput::LoadRecordingPreset()
{
	const char *quality = config_get_string(main->Config(), "SimpleOutput",
			"RecQuality");
	const char *encoder = config_get_string(main->Config(), "SimpleOutput",
			"RecEncoder");

	videoEncoder = encoder;
	videoQuality = quality;
	ffmpegOutput = false;

	if (strcmp(quality, "Stream") == 0) {
		h264Recording = h264Streaming;
		aacRecording = aacStreaming;
		usingRecordingPreset = false;
		return;

	} else if (strcmp(quality, "Lossless") == 0) {
		LoadRecordingPreset_Lossless();
		usingRecordingPreset = true;
		ffmpegOutput = true;
		return;

	} else {
		lowCPUx264 = false;

		if (strcmp(encoder, SIMPLE_ENCODER_X264) == 0) {
			LoadRecordingPreset_h264("obs_x264");
		} else if (strcmp(encoder, SIMPLE_ENCODER_X264_LOWCPU) == 0) {
			LoadRecordingPreset_h264("obs_x264");
			lowCPUx264 = true;
		} else if (strcmp(encoder, SIMPLE_ENCODER_QSV) == 0) {
			LoadRecordingPreset_h264("obs_qsv11");
		} else if (strcmp(encoder, SIMPLE_ENCODER_NVENC) == 0) {
			LoadRecordingPreset_h264("ffmpeg_nvenc");
		}
		usingRecordingPreset = true;

		if (!CreateAACEncoder(aacRecording, aacRecEncID, 192,
					"simple_aac_recording", 0))
			throw "Failed to create aac recording encoder "
			      "(simple output)";
	}
}

SimpleOutput::SimpleOutput(OBSBasic *main_) : BasicOutputHandler(main_)
{
	streamOutput = obs_output_create("rtmp_output", "simple_stream",
			nullptr, nullptr);
	if (!streamOutput)
		throw "Failed to create stream output (simple output)";
	obs_output_release(streamOutput);

	const char *encoder = config_get_string(main->Config(), "SimpleOutput",
			"StreamEncoder");
	if (strcmp(encoder, SIMPLE_ENCODER_QSV) == 0)
		LoadStreamingPreset_h264("obs_qsv11");
	else if (strcmp(encoder, SIMPLE_ENCODER_NVENC) == 0)
		LoadStreamingPreset_h264("ffmpeg_nvenc");
	else
		LoadStreamingPreset_h264("obs_x264");

	if (!CreateAACEncoder(aacStreaming, aacStreamEncID, GetAudioBitrate(),
				"simple_aac", 0))
		throw "Failed to create aac streaming encoder (simple output)";

	streamDelayStarting.Connect(obs_output_get_signal_handler(streamOutput),
			"starting", OBSStreamStarting, this);
	streamStopping.Connect(obs_output_get_signal_handler(streamOutput),
			"stopping", OBSStreamStopping, this);

	startStreaming.Connect(obs_output_get_signal_handler(streamOutput),
			"start", OBSStartStreaming, this);
	stopStreaming.Connect(obs_output_get_signal_handler(streamOutput),
			"stop", OBSStopStreaming, this);

	LoadRecordingPreset();

	if (!ffmpegOutput) {
		fileOutput = obs_output_create("ffmpeg_muxer",
				"simple_file_output", nullptr, nullptr);
		if (!fileOutput)
			throw "Failed to create recording output "
			      "(simple output)";
		obs_output_release(fileOutput);
	}
}

int SimpleOutput::GetAudioBitrate() const
{
	int bitrate = (int)config_get_uint(main->Config(), "SimpleOutput",
			"ABitrate");

	return FindClosestAvailableAACBitrate(bitrate);
}

void SimpleOutput::Update()
{
	obs_data_t *h264Settings = obs_data_create();
	obs_data_t *aacSettings  = obs_data_create();

	int videoBitrate = config_get_uint(main->Config(), "SimpleOutput",
			"VBitrate");
	int audioBitrate = GetAudioBitrate();
	bool advanced = config_get_bool(main->Config(), "SimpleOutput",
			"UseAdvanced");
	bool enforceBitrate = config_get_bool(main->Config(), "SimpleOutput",
			"EnforceBitrate");
	const char *custom = config_get_string(main->Config(),
			"SimpleOutput", "x264Settings");
	const char *encoder = config_get_string(main->Config(), "SimpleOutput",
			"StreamEncoder");
	const char *presetType;
	const char *preset;

	if (strcmp(encoder, SIMPLE_ENCODER_QSV) == 0)
		presetType = "QSVPreset";
	else if (strcmp(encoder, SIMPLE_ENCODER_NVENC) == 0)
		presetType = "NVENCPreset";
	else
		presetType = "Preset";

	preset = config_get_string(main->Config(), "SimpleOutput", presetType);

	obs_data_set_string(h264Settings, "rate_control", "CBR");
	obs_data_set_int(h264Settings, "bitrate", videoBitrate);

	if (advanced) {
		obs_data_set_string(h264Settings, "preset", preset);
		obs_data_set_string(h264Settings, "x264opts", custom);
	}

	obs_data_set_string(aacSettings, "rate_control", "CBR");
	obs_data_set_int(aacSettings, "bitrate", audioBitrate);

	obs_service_apply_encoder_settings(main->GetService(),
			h264Settings, aacSettings);

	if (advanced && !enforceBitrate) {
		obs_data_set_int(h264Settings, "bitrate", videoBitrate);
		obs_data_set_int(aacSettings, "bitrate", audioBitrate);
	}

	video_t *video = obs_get_video();
	enum video_format format = video_output_get_format(video);

	if (format != VIDEO_FORMAT_NV12 && format != VIDEO_FORMAT_I420)
		obs_encoder_set_preferred_video_format(h264Streaming,
				VIDEO_FORMAT_NV12);

	obs_encoder_update(h264Streaming, h264Settings);
	obs_encoder_update(aacStreaming,  aacSettings);

	obs_data_release(h264Settings);
	obs_data_release(aacSettings);
}

void SimpleOutput::UpdateRecordingAudioSettings()
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", 192);
	obs_data_set_string(settings, "rate_control", "CBR");

	obs_encoder_update(aacRecording, settings);

	obs_data_release(settings);
}

#define CROSS_DIST_CUTOFF 2000.0

int SimpleOutput::CalcCRF(int crf)
{
	int cx = config_get_uint(main->Config(), "Video", "OutputCX");
	int cy = config_get_uint(main->Config(), "Video", "OutputCY");
	double fCX = double(cx);
	double fCY = double(cy);

	if (lowCPUx264)
		crf -= 2;

	double crossDist = sqrt(fCX * fCX + fCY * fCY);
	double crfResReduction =
		fmin(CROSS_DIST_CUTOFF, crossDist) / CROSS_DIST_CUTOFF;
	crfResReduction = (1.0 - crfResReduction) * 10.0;

	return crf - int(crfResReduction);
}

void SimpleOutput::UpdateRecordingSettings_x264_crf(int crf)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "crf", crf);
	obs_data_set_bool(settings, "use_bufsize", true);
	obs_data_set_string(settings, "rate_control", "CRF");
	obs_data_set_string(settings, "profile", "high");
	obs_data_set_string(settings, "preset",
			lowCPUx264 ? "ultrafast" : "veryfast");

	obs_encoder_update(h264Recording, settings);

	obs_data_release(settings);
}

static bool icq_available(obs_encoder_t *encoder)
{
	obs_properties_t *props = obs_encoder_properties(encoder);
	obs_property_t *p = obs_properties_get(props, "rate_control");
	bool icq_found = false;

	size_t num = obs_property_list_item_count(p);
	for (size_t i = 0; i < num; i++) {
		const char *val = obs_property_list_item_string(p, i);
		if (strcmp(val, "ICQ") == 0) {
			icq_found = true;
			break;
		}
	}

	obs_properties_destroy(props);
	return icq_found;
}

void SimpleOutput::UpdateRecordingSettings_qsv11(int crf)
{
	bool icq = icq_available(h264Recording);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "profile", "high");

	if (icq) {
		obs_data_set_string(settings, "rate_control", "ICQ");
		obs_data_set_int(settings, "icq_quality", crf);
	} else {
		obs_data_set_string(settings, "rate_control", "CQP");
		obs_data_set_int(settings, "qpi", crf);
		obs_data_set_int(settings, "qpp", crf);
		obs_data_set_int(settings, "qpb", crf);
	}

	obs_encoder_update(h264Recording, settings);

	obs_data_release(settings);
}

void SimpleOutput::UpdateRecordingSettings_nvenc(int cqp)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CQP");
	obs_data_set_string(settings, "profile", "high");
	obs_data_set_string(settings, "preset", "hq");
	obs_data_set_int(settings, "cqp", cqp);

	obs_encoder_update(h264Recording, settings);

	obs_data_release(settings);
}

void SimpleOutput::UpdateRecordingSettings()
{
	bool ultra_hq = (videoQuality == "HQ");
	int crf = CalcCRF(ultra_hq ? 16 : 23);

	if (astrcmp_n(videoEncoder.c_str(), "x264", 4) == 0) {
		UpdateRecordingSettings_x264_crf(crf);

	} else if (videoEncoder == SIMPLE_ENCODER_QSV) {
		UpdateRecordingSettings_qsv11(crf);

	} else if (videoEncoder == SIMPLE_ENCODER_NVENC) {
		UpdateRecordingSettings_nvenc(crf);
	}
}

inline void SimpleOutput::SetupOutputs()
{
	SimpleOutput::Update();
	obs_encoder_set_video(h264Streaming, obs_get_video());
	obs_encoder_set_audio(aacStreaming,  obs_get_audio());

	if (usingRecordingPreset) {
		if (ffmpegOutput) {
			obs_output_set_media(fileOutput, obs_get_video(),
					obs_get_audio());
		} else {
			obs_encoder_set_video(h264Recording, obs_get_video());
			obs_encoder_set_audio(aacRecording,  obs_get_audio());
		}
	}
}

bool SimpleOutput::StartStreaming(obs_service_t *service)
{
	if (!Active())
		SetupOutputs();

	obs_output_set_video_encoder(streamOutput, h264Streaming);
	obs_output_set_audio_encoder(streamOutput, aacStreaming, 0);
	obs_output_set_service(streamOutput, service);

	bool reconnect = config_get_bool(main->Config(), "Output",
			"Reconnect");
	int retryDelay = config_get_uint(main->Config(), "Output",
			"RetryDelay");
	int maxRetries = config_get_uint(main->Config(), "Output",
			"MaxRetries");
	bool useDelay = config_get_bool(main->Config(), "Output",
			"DelayEnable");
	int delaySec = config_get_int(main->Config(), "Output",
			"DelaySec");
	bool preserveDelay = config_get_bool(main->Config(), "Output",
			"DelayPreserve");
	const char *bindIP = config_get_string(main->Config(), "Output",
			"BindIP");

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "bind_ip", bindIP);
	obs_output_update(streamOutput, settings);
	obs_data_release(settings);

	if (!reconnect)
		maxRetries = 0;

	obs_output_set_delay(streamOutput, useDelay ? delaySec : 0,
			preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);

	obs_output_set_reconnect_settings(streamOutput, maxRetries,
			retryDelay);

	if (obs_output_start(streamOutput)) {
		return true;
	}

	return false;
}

static void ensure_directory_exists(string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == string::npos)
		return;

	string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

bool SimpleOutput::StartRecording()
{
	if (usingRecordingPreset) {
		if (!ffmpegOutput)
			UpdateRecordingSettings();
	} else if (!obs_output_active(streamOutput)) {
		Update();
	}

	if (!Active())
		SetupOutputs();

	const char *path = config_get_string(main->Config(),
			"SimpleOutput", "FilePath");
	const char *format = config_get_string(main->Config(),
			"SimpleOutput", "RecFormat");
	const char *mux = config_get_string(main->Config(), "SimpleOutput",
			"MuxerCustom");
	bool noSpace = config_get_bool(main->Config(), "SimpleOutput",
			"FileNameWithoutSpace");
	const char *filenameFormat = config_get_string(main->Config(), "Output",
				"FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output",
				"OverwriteIfExists");

	os_dir_t *dir = path ? os_opendir(path) : nullptr;

	if (!dir) {
		QMessageBox::information(main,
				QTStr("Output.BadPath.Title"),
				QTStr("Output.BadPath.Text"));
		return false;
	}

	os_closedir(dir);

	string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\')
		strPath += "/";

	strPath += GenerateSpecifiedFilename(ffmpegOutput ? "avi" : format,
			noSpace, filenameFormat);
	ensure_directory_exists(strPath);
	if (!overwriteIfExists)
		FindBestFilename(strPath, noSpace);

	if (!ffmpegOutput) {
		obs_output_set_video_encoder(fileOutput, h264Recording);
		obs_output_set_audio_encoder(fileOutput, aacRecording, 0);
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, ffmpegOutput ? "url" : "path",
			strPath.c_str());
	obs_data_set_string(settings, "muxer_settings", mux);

	obs_output_update(fileOutput, settings);

	obs_data_release(settings);

	if (obs_output_start(fileOutput)) {
		return true;
	}

	return false;
}

void SimpleOutput::StopStreaming()
{
	obs_output_stop(streamOutput);
}

void SimpleOutput::ForceStopStreaming()
{
	obs_output_force_stop(streamOutput);
}

void SimpleOutput::StopRecording()
{
	obs_output_stop(fileOutput);
}

bool SimpleOutput::StreamingActive() const
{
	return obs_output_active(streamOutput);
}

bool SimpleOutput::RecordingActive() const
{
	return obs_output_active(fileOutput);
}

/* ------------------------------------------------------------------------ */

struct AdvancedOutput : BasicOutputHandler {
	OBSEncoder             aacTrack[4];
	OBSEncoder             h264Streaming;
	OBSEncoder             h264Recording;

	bool                   ffmpegOutput;
	bool                   ffmpegRecording;
	bool                   useStreamEncoder;

	string                 aacEncoderID[4];

	AdvancedOutput(OBSBasic *main_);

	inline void UpdateStreamSettings();
	inline void UpdateRecordingSettings();
	inline void UpdateAudioSettings();
	virtual void Update() override;

	inline void SetupStreaming();
	inline void SetupRecording();
	inline void SetupFFmpeg();
	void SetupOutputs();
	int GetAudioBitrate(size_t i) const;

	virtual bool StartStreaming(obs_service_t *service) override;
	virtual bool StartRecording() override;
	virtual void StopStreaming() override;
	virtual void ForceStopStreaming() override;
	virtual void StopRecording() override;
	virtual bool StreamingActive() const override;
	virtual bool RecordingActive() const override;
};

static OBSData GetDataFromJsonFile(const char *jsonFile)
{
	char fullPath[512];

	int ret = GetProfilePath(fullPath, sizeof(fullPath), jsonFile);
	if (ret > 0) {
		BPtr<char> jsonData = os_quick_read_utf8_file(fullPath);
		if (!!jsonData) {
			obs_data_t *data = obs_data_create_from_json(jsonData);
			OBSData dataRet(data);
			obs_data_release(data);
			return dataRet;
		}
	}

	return nullptr;
}

AdvancedOutput::AdvancedOutput(OBSBasic *main_) : BasicOutputHandler(main_)
{
	const char *recType = config_get_string(main->Config(), "AdvOut",
			"RecType");
	const char *streamEncoder = config_get_string(main->Config(), "AdvOut",
			"Encoder");

	ffmpegOutput = astrcmpi(recType, "FFmpeg") == 0;
	ffmpegRecording = ffmpegOutput &&
		config_get_bool(main->Config(), "AdvOut", "FFOutputToFile");
	useStreamEncoder = true;

	OBSData streamEncSettings = GetDataFromJsonFile("streamEncoder.json");
	OBSData recordEncSettings = GetDataFromJsonFile("recordEncoder.json");

	streamOutput = obs_output_create("rtmp_output", "adv_stream",
			nullptr, nullptr);
	if (!streamOutput)
		throw "Failed to create stream output (advanced output)";
	obs_output_release(streamOutput);

	if (ffmpegOutput) {
		fileOutput = obs_output_create("ffmpeg_output",
				"adv_ffmpeg_output", nullptr, nullptr);
		if (!fileOutput)
			throw "Failed to create recording FFmpeg output "
			      "(advanced output)";
		obs_output_release(fileOutput);
	} else {
		fileOutput = obs_output_create("ffmpeg_muxer",
				"adv_file_output", nullptr, nullptr);
		if (!fileOutput)
			throw "Failed to create recording output "
			      "(advanced output)";
		obs_output_release(fileOutput);
	}

	h264Streaming = obs_video_encoder_create(streamEncoder,
			"streaming_h264", streamEncSettings, nullptr);
	if (!h264Streaming)
		throw "Failed to create streaming h264 encoder "
		      "(advanced output)";
	obs_encoder_release(h264Streaming);

	for (int i = 0; i < 4; i++) {
		char name[9];
		sprintf(name, "adv_aac%d", i);

		if (!CreateAACEncoder(aacTrack[i], aacEncoderID[i],
					GetAudioBitrate(i), name, i))
			throw "Failed to create audio encoder "
			      "(advanced output)";
	}

	streamDelayStarting.Connect(obs_output_get_signal_handler(streamOutput),
			"starting", OBSStreamStarting, this);
	streamStopping.Connect(obs_output_get_signal_handler(streamOutput),
			"stopping", OBSStreamStopping, this);

	startStreaming.Connect(obs_output_get_signal_handler(streamOutput),
			"start", OBSStartStreaming, this);
	stopStreaming.Connect(obs_output_get_signal_handler(streamOutput),
			"stop", OBSStopStreaming, this);

	recordStopping.Connect(obs_output_get_signal_handler(fileOutput),
			"stopping", OBSRecordStopping, this);
}

void AdvancedOutput::UpdateStreamSettings()
{
	bool applyServiceSettings = config_get_bool(main->Config(), "AdvOut",
			"ApplyServiceSettings");

	OBSData settings = GetDataFromJsonFile("streamEncoder.json");

	if (applyServiceSettings)
		obs_service_apply_encoder_settings(main->GetService(),
				settings, nullptr);

	video_t *video = obs_get_video();
	enum video_format format = video_output_get_format(video);

	if (format != VIDEO_FORMAT_NV12 && format != VIDEO_FORMAT_I420)
		obs_encoder_set_preferred_video_format(h264Streaming,
				VIDEO_FORMAT_NV12);

	obs_encoder_update(h264Streaming, settings);
}

inline void AdvancedOutput::UpdateRecordingSettings()
{
	OBSData settings = GetDataFromJsonFile("recordEncoder.json");
	obs_encoder_update(h264Recording, settings);
}

void AdvancedOutput::Update()
{
	UpdateStreamSettings();
	if (!useStreamEncoder && !ffmpegOutput)
		UpdateRecordingSettings();
	UpdateAudioSettings();
}

inline void AdvancedOutput::SetupStreaming()
{
	bool rescale = config_get_bool(main->Config(), "AdvOut",
			"Rescale");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut",
			"RescaleRes");
	bool multitrack = config_get_bool(main->Config(), "AdvOut",
			"Multitrack");
	int trackIndex = config_get_int(main->Config(), "AdvOut",
			"TrackIndex");
	int trackCount = config_get_int(main->Config(), "AdvOut",
			"TrackCount");
	unsigned int cx = 0;
	unsigned int cy = 0;

	if (rescale && rescaleRes && *rescaleRes) {
		if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
			cx = 0;
			cy = 0;
		}
	}

	obs_encoder_set_scaled_size(h264Streaming, cx, cy);
	obs_encoder_set_video(h264Streaming, obs_get_video());

	obs_output_set_video_encoder(streamOutput, h264Streaming);

	if (multitrack) {
		int i = 0;
		for (; i < trackCount; i++)
			obs_output_set_audio_encoder(streamOutput, aacTrack[i],
					i);
		for (; i < 4; i++)
			obs_output_set_audio_encoder(streamOutput, nullptr, i);

	} else {
		obs_output_set_audio_encoder(streamOutput,
				aacTrack[trackIndex - 1], 0);
	}
}

inline void AdvancedOutput::SetupRecording()
{
	const char *path = config_get_string(main->Config(), "AdvOut",
			"RecFilePath");
	const char *mux = config_get_string(main->Config(), "AdvOut",
			"RecMuxerCustom");
	bool rescale = config_get_bool(main->Config(), "AdvOut",
			"RecRescale");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut",
			"RecRescaleRes");
	int tracks = config_get_int(main->Config(), "AdvOut", "RecTracks");
	obs_data_t *settings = obs_data_create();
	unsigned int cx = 0;
	unsigned int cy = 0;
	int idx = 0;

	if (useStreamEncoder) {
		obs_output_set_video_encoder(fileOutput, h264Streaming);
	} else {
		if (rescale && rescaleRes && *rescaleRes) {
			if (sscanf(rescaleRes, "%ux%u", &cx, &cy) != 2) {
				cx = 0;
				cy = 0;
			}
		}

		obs_encoder_set_scaled_size(h264Recording, cx, cy);
		obs_encoder_set_video(h264Recording, obs_get_video());
		obs_output_set_video_encoder(fileOutput, h264Recording);
	}

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if ((tracks & (1<<i)) != 0) {
			obs_output_set_audio_encoder(fileOutput, aacTrack[i],
					idx++);
		}
	}

	obs_data_set_string(settings, "path", path);
	obs_data_set_string(settings, "muxer_settings", mux);
	obs_output_update(fileOutput, settings);
	obs_data_release(settings);
}

inline void AdvancedOutput::SetupFFmpeg()
{
	const char *url = config_get_string(main->Config(), "AdvOut", "FFURL");
	int vBitrate = config_get_int(main->Config(), "AdvOut",
			"FFVBitrate");
	bool rescale = config_get_bool(main->Config(), "AdvOut",
			"FFRescale");
	const char *rescaleRes = config_get_string(main->Config(), "AdvOut",
			"FFRescaleRes");
	const char *formatName = config_get_string(main->Config(), "AdvOut",
			"FFFormat");
	const char *mimeType = config_get_string(main->Config(), "AdvOut",
			"FFFormatMimeType");
	const char *muxCustom = config_get_string(main->Config(), "AdvOut",
			"FFMCustom");
	const char *vEncoder = config_get_string(main->Config(), "AdvOut",
			"FFVEncoder");
	int vEncoderId = config_get_int(main->Config(), "AdvOut",
			"FFVEncoderId");
	const char *vEncCustom = config_get_string(main->Config(), "AdvOut",
			"FFVCustom");
	int aBitrate = config_get_int(main->Config(), "AdvOut",
			"FFABitrate");
	int aTrack = config_get_int(main->Config(), "AdvOut",
			"FFAudioTrack");
	const char *aEncoder = config_get_string(main->Config(), "AdvOut",
			"FFAEncoder");
	int aEncoderId = config_get_int(main->Config(), "AdvOut",
			"FFAEncoderId");
	const char *aEncCustom = config_get_string(main->Config(), "AdvOut",
			"FFACustom");
	obs_data_t *settings = obs_data_create();

	obs_data_set_string(settings, "url", url);
	obs_data_set_string(settings, "format_name", formatName);
	obs_data_set_string(settings, "format_mime_type", mimeType);
	obs_data_set_string(settings, "muxer_settings", muxCustom);
	obs_data_set_int(settings, "video_bitrate", vBitrate);
	obs_data_set_string(settings, "video_encoder", vEncoder);
	obs_data_set_int(settings, "video_encoder_id", vEncoderId);
	obs_data_set_string(settings, "video_settings", vEncCustom);
	obs_data_set_int(settings, "audio_bitrate", aBitrate);
	obs_data_set_string(settings, "audio_encoder", aEncoder);
	obs_data_set_int(settings, "audio_encoder_id", aEncoderId);
	obs_data_set_string(settings, "audio_settings", aEncCustom);

	if (rescale && rescaleRes && *rescaleRes) {
		int width;
		int height;
		int val = sscanf(rescaleRes, "%dx%d", &width, &height);

		if (val == 2 && width && height) {
			obs_data_set_int(settings, "scale_width", width);
			obs_data_set_int(settings, "scale_height", height);
		}
	}

	obs_output_set_mixer(fileOutput, aTrack - 1);
	obs_output_set_media(fileOutput, obs_get_video(), obs_get_audio());
	obs_output_update(fileOutput, settings);

	obs_data_release(settings);
}

static inline void SetEncoderName(obs_encoder_t *encoder, const char *name,
		const char *defaultName)
{
	obs_encoder_set_name(encoder, (name && *name) ? name : defaultName);
}

inline void AdvancedOutput::UpdateAudioSettings()
{
	const char *name1 = config_get_string(main->Config(), "AdvOut",
			"Track1Name");
	const char *name2 = config_get_string(main->Config(), "AdvOut",
			"Track2Name");
	const char *name3 = config_get_string(main->Config(), "AdvOut",
			"Track3Name");
	const char *name4 = config_get_string(main->Config(), "AdvOut",
			"Track4Name");
	bool applyServiceSettings = config_get_bool(main->Config(), "AdvOut",
			"ApplyServiceSettings");
	int streamTrackIndex = config_get_int(main->Config(), "AdvOut",
			"TrackIndex");
	obs_data_t *settings[4];

	for (size_t i = 0; i < 4; i++) {
		settings[i] = obs_data_create();
		obs_data_set_int(settings[i], "bitrate", GetAudioBitrate(i));
	}

	SetEncoderName(aacTrack[0], name1, "Track1");
	SetEncoderName(aacTrack[1], name2, "Track2");
	SetEncoderName(aacTrack[2], name3, "Track3");
	SetEncoderName(aacTrack[3], name4, "Track4");

	for (size_t i = 0; i < 4; i++) {
		if (applyServiceSettings && (int)(i + 1) == streamTrackIndex)
			obs_service_apply_encoder_settings(main->GetService(),
					nullptr, settings[i]);

		obs_encoder_update(aacTrack[i], settings[i]);
		obs_data_release(settings[i]);
	}
}

void AdvancedOutput::SetupOutputs()
{
	obs_encoder_set_video(h264Streaming, obs_get_video());
	if (h264Recording)
		obs_encoder_set_video(h264Recording, obs_get_video());
	obs_encoder_set_audio(aacTrack[0], obs_get_audio());
	obs_encoder_set_audio(aacTrack[1], obs_get_audio());
	obs_encoder_set_audio(aacTrack[2], obs_get_audio());
	obs_encoder_set_audio(aacTrack[3], obs_get_audio());

	SetupStreaming();

	if (ffmpegOutput)
		SetupFFmpeg();
	else
		SetupRecording();
}

int AdvancedOutput::GetAudioBitrate(size_t i) const
{
	const char *names[] = {
		"Track1Bitrate", "Track2Bitrate",
		"Track3Bitrate", "Track4Bitrate",
	};
	int bitrate = (int)config_get_uint(main->Config(), "AdvOut", names[i]);
	return FindClosestAvailableAACBitrate(bitrate);
}

bool AdvancedOutput::StartStreaming(obs_service_t *service)
{
	if (!useStreamEncoder ||
	    (!ffmpegOutput && !obs_output_active(fileOutput))) {
		UpdateStreamSettings();
	}

	UpdateAudioSettings();

	if (!Active())
		SetupOutputs();

	obs_output_set_service(streamOutput, service);

	bool reconnect = config_get_bool(main->Config(), "Output", "Reconnect");
	int retryDelay = config_get_int(main->Config(), "Output", "RetryDelay");
	int maxRetries = config_get_int(main->Config(), "Output", "MaxRetries");
	bool useDelay = config_get_bool(main->Config(), "Output",
			"DelayEnable");
	int delaySec = config_get_int(main->Config(), "Output",
			"DelaySec");
	bool preserveDelay = config_get_bool(main->Config(), "Output",
			"DelayPreserve");
	const char *bindIP = config_get_string(main->Config(), "Output",
			"BindIP");

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "bind_ip", bindIP);
	obs_output_update(streamOutput, settings);
	obs_data_release(settings);

	if (!reconnect)
		maxRetries = 0;

	obs_output_set_delay(streamOutput, useDelay ? delaySec : 0,
			preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);

	obs_output_set_reconnect_settings(streamOutput, maxRetries,
			retryDelay);

	if (obs_output_start(streamOutput)) {
		return true;
	}

	return false;
}

bool AdvancedOutput::StartRecording()
{
	const char *path;
	const char *recFormat;
	const char *filenameFormat;
	bool noSpace = false;
	bool overwriteIfExists = false;

	if (!useStreamEncoder) {
		if (!ffmpegOutput) {
			UpdateRecordingSettings();
		}
	} else if (!obs_output_active(streamOutput)) {
		UpdateStreamSettings();
	}

	UpdateAudioSettings();

	if (!Active())
		SetupOutputs();

	if (!ffmpegOutput || ffmpegRecording) {
		path = config_get_string(main->Config(), "AdvOut",
				ffmpegRecording ? "FFFilePath" : "RecFilePath");
		recFormat = config_get_string(main->Config(), "AdvOut",
				ffmpegRecording ? "FFExtension" : "RecFormat");
		filenameFormat = config_get_string(main->Config(), "Output",
				"FilenameFormatting");
		overwriteIfExists = config_get_bool(main->Config(), "Output",
				"OverwriteIfExists");
		noSpace = config_get_bool(main->Config(), "AdvOut",
				ffmpegRecording ?
				"FFFileNameWithoutSpace" :
				"RecFileNameWithoutSpace");

		os_dir_t *dir = path ? os_opendir(path) : nullptr;

		if (!dir) {
			QMessageBox::information(main,
					QTStr("Output.BadPath.Title"),
					QTStr("Output.BadPath.Text"));
			return false;
		}

		os_closedir(dir);

		string strPath;
		strPath += path;

		char lastChar = strPath.back();
		if (lastChar != '/' && lastChar != '\\')
			strPath += "/";

		strPath += GenerateSpecifiedFilename(recFormat, noSpace,
							filenameFormat);
		ensure_directory_exists(strPath);
		if (!overwriteIfExists)
			FindBestFilename(strPath, noSpace);

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings,
				ffmpegRecording ? "url" : "path",
				strPath.c_str());

		obs_output_update(fileOutput, settings);

		obs_data_release(settings);
	}

	if (obs_output_start(fileOutput)) {
		return true;
	}

	return false;
}

void AdvancedOutput::StopStreaming()
{
	obs_output_stop(streamOutput);
}

void AdvancedOutput::ForceStopStreaming()
{
	obs_output_force_stop(streamOutput);
}

void AdvancedOutput::StopRecording()
{
	obs_output_stop(fileOutput);
}

bool AdvancedOutput::StreamingActive() const
{
	return obs_output_active(streamOutput);
}

bool AdvancedOutput::RecordingActive() const
{
	return obs_output_active(fileOutput);
}

/* ------------------------------------------------------------------------ */

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main)
{
	return new SimpleOutput(main);
}

BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main)
{
	return new AdvancedOutput(main);
}
