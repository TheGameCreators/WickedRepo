#include "wiAudio.h"
#include "wiBackLog.h"
#include "wiHelper.h"

#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include "Utility/stb_vorbis.c"

#ifdef _WIN32

#include <wrl/client.h> // ComPtr
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <x3daudio.h>
#pragma comment(lib,"xaudio2.lib")

//Little-Endian things:
#define fourccRIFF 'FFIR'
#define fourccDATA 'atad'
#define fourccFMT ' tmf'
#define fourccWAVE 'EVAW'
#define fourccXWMA 'AMWX'
#define fourccDPDS 'sdpd'

template<typename T>
static constexpr T AlignTo(T value, T alignment)
{
	return ((value + alignment - T(1)) / alignment) * alignment;
}

namespace wiAudio
{
	static const XAUDIO2FX_REVERB_I3DL2_PARAMETERS reverbPresets[] =
	{
		XAUDIO2FX_I3DL2_PRESET_DEFAULT,
		XAUDIO2FX_I3DL2_PRESET_GENERIC,
		XAUDIO2FX_I3DL2_PRESET_FOREST,
		XAUDIO2FX_I3DL2_PRESET_PADDEDCELL,
		XAUDIO2FX_I3DL2_PRESET_ROOM,
		XAUDIO2FX_I3DL2_PRESET_BATHROOM,
		XAUDIO2FX_I3DL2_PRESET_LIVINGROOM,
		XAUDIO2FX_I3DL2_PRESET_STONEROOM,
		XAUDIO2FX_I3DL2_PRESET_AUDITORIUM,
		XAUDIO2FX_I3DL2_PRESET_CONCERTHALL,
		XAUDIO2FX_I3DL2_PRESET_CAVE,
		XAUDIO2FX_I3DL2_PRESET_ARENA,
		XAUDIO2FX_I3DL2_PRESET_HANGAR,
		XAUDIO2FX_I3DL2_PRESET_CARPETEDHALLWAY,
		XAUDIO2FX_I3DL2_PRESET_HALLWAY,
		XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR,
		XAUDIO2FX_I3DL2_PRESET_ALLEY,
		XAUDIO2FX_I3DL2_PRESET_CITY,
		XAUDIO2FX_I3DL2_PRESET_MOUNTAINS,
		XAUDIO2FX_I3DL2_PRESET_QUARRY,
		XAUDIO2FX_I3DL2_PRESET_PLAIN,
		XAUDIO2FX_I3DL2_PRESET_PARKINGLOT,
		XAUDIO2FX_I3DL2_PRESET_SEWERPIPE,
		XAUDIO2FX_I3DL2_PRESET_UNDERWATER,
		XAUDIO2FX_I3DL2_PRESET_SMALLROOM,
		XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM,
		XAUDIO2FX_I3DL2_PRESET_LARGEROOM,
		XAUDIO2FX_I3DL2_PRESET_MEDIUMHALL,
		XAUDIO2FX_I3DL2_PRESET_LARGEHALL,
		XAUDIO2FX_I3DL2_PRESET_PLATE,
	};

	struct AudioInternal
	{
		bool success = false;
		Microsoft::WRL::ComPtr<IXAudio2> audioEngine;
		IXAudio2MasteringVoice* masteringVoice = nullptr;
		XAUDIO2_VOICE_DETAILS masteringVoiceDetails = {};
		IXAudio2SubmixVoice* submixVoices[SUBMIX_TYPE_COUNT] = {};
		X3DAUDIO_HANDLE audio3D = {};
		Microsoft::WRL::ComPtr<IUnknown> reverbEffect;
		IXAudio2SubmixVoice* reverbSubmix = nullptr;
		uint32_t termination_data = 0;
		XAUDIO2_BUFFER termination_mark = {};

		AudioInternal()
		{
			HRESULT hr;
			hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
			assert(SUCCEEDED(hr));

			hr = XAudio2Create(&audioEngine, 0, XAUDIO2_DEFAULT_PROCESSOR);
			assert(SUCCEEDED(hr));

#ifdef _DEBUG
			XAUDIO2_DEBUG_CONFIGURATION debugConfig = {};
			debugConfig.TraceMask = XAUDIO2_LOG_ERRORS | XAUDIO2_LOG_WARNINGS;
			debugConfig.BreakMask = XAUDIO2_LOG_ERRORS | XAUDIO2_LOG_WARNINGS;
			audioEngine->SetDebugConfiguration(&debugConfig);
#endif // _DEBUG

			hr = audioEngine->CreateMasteringVoice(&masteringVoice);
			assert(SUCCEEDED(hr));

			if (masteringVoice == nullptr)
			{
				wiBackLog::post("Failed to create XAudio2 mastering voice!");
				return;
			}

			masteringVoice->GetVoiceDetails(&masteringVoiceDetails);

			// Without clamping sample rate, it was crashing 32bit 192kHz audio devices
			if (masteringVoiceDetails.InputSampleRate > 48000)
				masteringVoiceDetails.InputSampleRate = 48000;

			for (int i = 0; i < SUBMIX_TYPE_COUNT; ++i)
			{
				hr = audioEngine->CreateSubmixVoice(
					&submixVoices[i],
					masteringVoiceDetails.InputChannels,
					masteringVoiceDetails.InputSampleRate,
					0, 0, 0, 0);
				assert(SUCCEEDED(hr));
			}

			DWORD channelMask;
			masteringVoice->GetChannelMask(&channelMask);
			hr = X3DAudioInitialize(channelMask, X3DAUDIO_SPEED_OF_SOUND, audio3D);
			assert(SUCCEEDED(hr));

			// Reverb setup:
			{
				hr = XAudio2CreateReverb(&reverbEffect);
				assert(SUCCEEDED(hr));

				XAUDIO2_EFFECT_DESCRIPTOR effects[] = { { reverbEffect.Get(), TRUE, 1 } };
				XAUDIO2_EFFECT_CHAIN effectChain = { arraysize(effects), effects };
				hr = audioEngine->CreateSubmixVoice(
					&reverbSubmix,
					1, // reverb is mono
					masteringVoiceDetails.InputSampleRate,
					0, 0, nullptr, &effectChain);
#ifdef GGREDUCED
				if (SUCCEEDED(hr))
				{
					XAUDIO2FX_REVERB_PARAMETERS native;
					ReverbConvertI3DL2ToNative(&reverbPresets[REVERB_PRESET_DEFAULT], &native);
					HRESULT hr = reverbSubmix->SetEffectParameters(0, &native, sizeof(native));
					assert(SUCCEEDED(hr));
				}
				else
				{
					// This can fail on some systems
					// (reverbSubmix==NULL)
				}
#else
				assert(SUCCEEDED(hr));
				XAUDIO2FX_REVERB_PARAMETERS native;
				ReverbConvertI3DL2ToNative(&reverbPresets[REVERB_PRESET_DEFAULT], &native);
				HRESULT hr = reverbSubmix->SetEffectParameters(0, &native, sizeof(native));
				assert(SUCCEEDED(hr));
#endif
			}

			termination_mark.Flags = XAUDIO2_END_OF_STREAM;
			termination_mark.pAudioData = (const BYTE*)&termination_data;
			termination_mark.AudioBytes = sizeof(termination_data);
			success = SUCCEEDED(hr);
		}
		~AudioInternal()
		{

			if (reverbSubmix != nullptr)
				reverbSubmix->DestroyVoice();

			for (int i = 0; i < SUBMIX_TYPE_COUNT; ++i)
			{
				if (submixVoices[i] != nullptr)
					submixVoices[i]->DestroyVoice();
			}

			if (masteringVoice != nullptr)
				masteringVoice->DestroyVoice();

			audioEngine->StopEngine();

			CoUninitialize();
		}
	};
	std::shared_ptr<AudioInternal> audio;

	struct SoundInternal
	{
		std::shared_ptr<AudioInternal> audio;
		WAVEFORMATEX wfx = {};
		std::vector<uint8_t> audioData;
	};

	struct MyVoiceCallback : public IXAudio2VoiceCallback
	{
		bool m_isFinished = false;
		bool m_hasStarted = false;
		uint32_t iCallbackCountF = 0;
		uint32_t iCallbackCountS = 0;

		MyVoiceCallback() : m_isFinished(false), m_hasStarted(false) {}

		void OnVoiceProcessingPassStart(UINT32 /*BytesRequired*/) {}
		//void OnVoiceProcessingPassEnd() { m_isFinished = true; iCallbackCountF++; }
		void OnVoiceProcessingPassEnd() {}
		void OnStreamEnd() { m_isFinished = true; iCallbackCountF++; }
		void OnBufferStart(void* /*pBufferContext*/) { m_hasStarted = true; m_isFinished = false; iCallbackCountS++; } // Sound started
		void OnBufferEnd(void* /*pBufferContext*/) {}
		void OnLoopEnd(void* /*pBufferContext*/) {}
		void OnVoiceError(void* /*pBufferContext*/, HRESULT /*Error*/) {}
		void ResetFinished() { m_isFinished = false; } // Add reset function

		uint32_t GetCallBackF() const {	return iCallbackCountF; }
		uint32_t GetCallBackS() const { return iCallbackCountS; }
		bool IsFinished() const { return m_isFinished; }
		bool HasStarted() const { return m_hasStarted; }
	};

	struct SoundInstanceInternal
	{
		std::shared_ptr<AudioInternal> audio;
		std::shared_ptr<SoundInternal> soundinternal;
		IXAudio2SourceVoice* sourceVoice = nullptr;
		XAUDIO2_VOICE_DETAILS voiceDetails = {};
		std::vector<float> outputMatrix;
		std::vector<float> channelAzimuths;
		XAUDIO2_BUFFER buffer = {};
		uint32_t totalSamples = 0;
		MyVoiceCallback* pCallback = nullptr;

		~SoundInstanceInternal()
		{
			sourceVoice->Stop();
			sourceVoice->DestroyVoice();
		}
	};
	SoundInternal* to_internal(const Sound* param)
	{
		return static_cast<SoundInternal*>(param->internal_state.get());
	}
	SoundInstanceInternal* to_internal(const SoundInstance* param)
	{
		return static_cast<SoundInstanceInternal*>(param->internal_state.get());
	}

	void Initialize()
	{
		audio = std::make_shared<AudioInternal>();

		if (audio->success)
		{
			wiBackLog::post("wiAudio Initialized");
		}
	}

	bool FindChunk(const uint8_t* data, DWORD fourcc, DWORD& dwChunkSize, DWORD& dwChunkDataPosition)
	{
		size_t pos = 0;

		DWORD dwChunkType;
		DWORD dwChunkDataSize;
		DWORD dwRIFFDataSize = 0;
		DWORD dwFileType;
		DWORD bytesRead = 0;
		DWORD dwOffset = 0;

		while (true)
		{
			memcpy(&dwChunkType, data + pos, sizeof(DWORD));
			pos += sizeof(DWORD);
			memcpy(&dwChunkDataSize, data + pos, sizeof(DWORD));
			pos += sizeof(DWORD);

			switch (dwChunkType)
			{
			case fourccRIFF:
				dwRIFFDataSize = dwChunkDataSize;
				dwChunkDataSize = 4;
				memcpy(&dwFileType, data + pos, sizeof(DWORD));
				pos += sizeof(DWORD);
				break;

			default:
				pos += dwChunkDataSize;
			}

			dwOffset += sizeof(DWORD) * 2;

			if (dwChunkType == fourcc)
			{
				dwChunkSize = dwChunkDataSize;
				dwChunkDataPosition = dwOffset;
				return true;
			}

			dwOffset += dwChunkDataSize;

			if (bytesRead >= dwRIFFDataSize) return false;

		}

		return true;

	}

	bool CreateSound(const std::string& filename, Sound* sound)
	{
		std::vector<uint8_t> filedata;
		bool success = wiHelper::FileRead(filename, filedata);
		if (!success)
		{
			return false;
		}
		return CreateSound(filedata, sound);
	}
	bool CreateSound(const std::vector<uint8_t>& data, Sound* sound)
	{
		return CreateSound(data.data(), data.size(), sound);
	}
	bool CreateSound(const uint8_t* data, size_t size, Sound* sound)
	{
		std::shared_ptr<SoundInternal> soundinternal = std::make_shared<SoundInternal>();
		soundinternal->audio = audio;
		sound->internal_state = soundinternal;

		DWORD dwChunkSize;
		DWORD dwChunkPosition;

		bool success;

		success = FindChunk(data, fourccRIFF, dwChunkSize, dwChunkPosition);
		if (success)
		{
			// Wav decoder:
			DWORD filetype;
			memcpy(&filetype, data + dwChunkPosition, sizeof(DWORD));
			if (filetype != fourccWAVE)
			{
				assert(0);
				return false;
			}

			success = FindChunk(data, fourccFMT, dwChunkSize, dwChunkPosition);
			if (!success)
			{
				assert(0);
				return false;
			}
			memcpy(&soundinternal->wfx, data + dwChunkPosition, dwChunkSize);
			soundinternal->wfx.wFormatTag = WAVE_FORMAT_PCM;

			success = FindChunk(data, fourccDATA, dwChunkSize, dwChunkPosition);
			if (!success)
			{
				assert(0);
				return false;
			}

			soundinternal->audioData.resize(dwChunkSize);
			memcpy(soundinternal->audioData.data(), data + dwChunkPosition, dwChunkSize);
		}
		else
		{
			// Ogg decoder:
			int channels = 0;
			int sample_rate = 0;
			short* output = nullptr;
			int samples = stb_vorbis_decode_memory(data, (int)size, &channels, &sample_rate, &output);
			if (samples < 0)
			{
				assert(0);
				return false;
			}

			// WAVEFORMATEX: https://docs.microsoft.com/en-us/previous-versions/dd757713(v=vs.85)?redirectedfrom=MSDN
			soundinternal->wfx.wFormatTag = WAVE_FORMAT_PCM;
			soundinternal->wfx.nChannels = (WORD)channels;
			soundinternal->wfx.nSamplesPerSec = (DWORD)sample_rate;
			soundinternal->wfx.wBitsPerSample = sizeof(short) * 8;
			soundinternal->wfx.nBlockAlign = (WORD)channels * sizeof(short); // is this right?
			soundinternal->wfx.nAvgBytesPerSec = soundinternal->wfx.nSamplesPerSec * soundinternal->wfx.nBlockAlign;

			size_t output_size = size_t(samples * channels) * sizeof(short);
			soundinternal->audioData.resize(output_size);
			memcpy(soundinternal->audioData.data(), output, output_size);

			free(output);
		}

		return true;
	}
	bool CreateSoundInstance(const Sound* sound, SoundInstance* instance)
	{
		HRESULT hr;
		const auto& soundinternal = std::static_pointer_cast<SoundInternal>(sound->internal_state);
		std::shared_ptr<SoundInstanceInternal> instanceinternal = std::make_shared<SoundInstanceInternal>();
		instance->internal_state = instanceinternal;

		instanceinternal->audio = audio;
		instanceinternal->soundinternal = soundinternal;

		XAUDIO2_SEND_DESCRIPTOR SFXSend[] = {
			{ XAUDIO2_SEND_USEFILTER, audio->submixVoices[instance->type] },
			{ XAUDIO2_SEND_USEFILTER, audio->reverbSubmix }, // this should be last to enable/disable reverb simply
		};
		XAUDIO2_VOICE_SENDS SFXSendList = {
			instance->IsEnableReverb() ? arraysize(SFXSend) : 1,
			SFXSend
		};

		instanceinternal->pCallback = new MyVoiceCallback();

		hr = audio->audioEngine->CreateSourceVoice(&instanceinternal->sourceVoice, &soundinternal->wfx,
			0, XAUDIO2_DEFAULT_FREQ_RATIO, instanceinternal->pCallback, &SFXSendList, NULL);
		if (FAILED(hr))
		{
			assert(0);
			return false;
		}

		instanceinternal->sourceVoice->GetVoiceDetails(&instanceinternal->voiceDetails);

		instanceinternal->outputMatrix.resize(size_t(instanceinternal->voiceDetails.InputChannels) * size_t(audio->masteringVoiceDetails.InputChannels));
		instanceinternal->channelAzimuths.resize(instanceinternal->voiceDetails.InputChannels);
		for (size_t i = 0; i < instanceinternal->channelAzimuths.size(); ++i)
		{
			instanceinternal->channelAzimuths[i] = X3DAUDIO_2PI * float(i) / float(instanceinternal->channelAzimuths.size());
		}

		WORD wBitsPS = soundinternal->wfx.wBitsPerSample;
		const uint32_t bytes_per_second = soundinternal->wfx.nSamplesPerSec * soundinternal->wfx.nChannels * sizeof(short);
		//PE: Some ogg files are not aligned correct and SubmitSourceBuffer fail, align here:
		instanceinternal->totalSamples = (UINT32)soundinternal->audioData.size() / (instanceinternal->voiceDetails.InputChannels * (wBitsPS / 8));
		uint32_t totalAudiosize = instanceinternal->totalSamples * instanceinternal->voiceDetails.InputChannels * (wBitsPS / 8);
		instanceinternal->buffer.AudioBytes = totalAudiosize;
		instanceinternal->buffer.pAudioData = soundinternal->audioData.data();
		instanceinternal->buffer.Flags = XAUDIO2_END_OF_STREAM;
		instanceinternal->buffer.LoopCount = instance->IsLooped() ? XAUDIO2_LOOP_INFINITE : 0;
		uint32_t num_remaining_samples = instanceinternal->buffer.AudioBytes / (soundinternal->wfx.nChannels * sizeof(short));
		if (instance->loop_begin > 0)
		{
			instanceinternal->buffer.LoopBegin = AlignTo(std::min(num_remaining_samples, uint32_t(instance->loop_begin * soundinternal->wfx.nSamplesPerSec)), 4u);
			num_remaining_samples -= instanceinternal->buffer.LoopBegin;
		}
		instanceinternal->buffer.LoopLength = AlignTo(std::min(num_remaining_samples, uint32_t(instance->loop_length * soundinternal->wfx.nSamplesPerSec)), 4u);

		instanceinternal->sourceVoice->Stop(0);
		instanceinternal->sourceVoice->FlushSourceBuffers();
		hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&instanceinternal->buffer);
		if (FAILED(hr))
		{
			assert(0);
			return false;
		}

		return true;
	}

	void PlayLooping(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr;
			hr = instanceinternal->sourceVoice->FlushSourceBuffers(); // reset submitted audio buffer
			assert(SUCCEEDED(hr));
			hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&instanceinternal->buffer); // resubmit
			assert(SUCCEEDED(hr));
			hr = instanceinternal->sourceVoice->Start();
			assert(SUCCEEDED(hr));
		}
	}
	void Play(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr = instanceinternal->sourceVoice->Start();
			assert(SUCCEEDED(hr));
		}
	}

	uint32_t GetSoundChannels(SoundInstance* instance)
	{
		//instanceinternal->sourceVoice->GetVoiceDetails(&instanceinternal->voiceDetails);
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			return(instanceinternal->voiceDetails.InputChannels);
		}
		return 0;
	}
	uint32_t PlayedSamples(SoundInstance* instance)
	{
		XAUDIO2_VOICE_STATE state;
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			instanceinternal->sourceVoice->GetState(&state);
			return(state.SamplesPlayed);
		}
		return 0;
	}

	float PlayedPercent(SoundInstance* instance)
	{
		XAUDIO2_VOICE_STATE state;
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			instanceinternal->sourceVoice->GetState(&state);
			UINT64 samplesPlayed = state.SamplesPlayed;
			//PE: Calculate percent played
			float percent = 0.0f;
			if (instanceinternal->totalSamples > 0)
			{
				percent = ( (float) samplesPlayed / (float)instanceinternal->totalSamples) * 100.0f;
				return(percent);
			}
		}
		return 0;
	}
	bool GetVoiceState(SoundInstance* instance,void * state)
	{
		if (state)
		{
			if (instance != nullptr && instance->IsValid())
			{
				auto instanceinternal = to_internal(instance);
				instanceinternal->sourceVoice->GetState( (XAUDIO2_VOICE_STATE *) state);
				return true;
			}
		}
		return false;
	}
	bool bIsReallyPlaying(SoundInstance* instance)
	{
		XAUDIO2_VOICE_STATE state;
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			if (instanceinternal->pCallback)
			{
				if (instanceinternal->pCallback->HasStarted() && !instanceinternal->pCallback->IsFinished())
					return true;
				return false;
			}
		}
		return true;
	}

	uint32_t GetCallBackF(SoundInstance* instance)
	{
		XAUDIO2_VOICE_STATE state;
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			if (instanceinternal->pCallback)
			{
				return instanceinternal->pCallback->GetCallBackF();
			}
		}
		return 0;
	}
	uint32_t GetCallBackS(SoundInstance* instance)
	{
		XAUDIO2_VOICE_STATE state;
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			if (instanceinternal->pCallback)
			{
				return instanceinternal->pCallback->GetCallBackS();
			}
		}
		return 0;
	}

	void Pause(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr = instanceinternal->sourceVoice->Stop(); // preserves cursor position
			assert(SUCCEEDED(hr));
		}
	}
	void Stop(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr = instanceinternal->sourceVoice->Stop(); // preserves cursor position
			assert(SUCCEEDED(hr)); 
			hr = instanceinternal->sourceVoice->FlushSourceBuffers(); // reset submitted audio buffer
			assert(SUCCEEDED(hr));
			if( instanceinternal->pCallback && !instanceinternal->pCallback->IsFinished() )
			{
				#ifdef _DEBUG
				// exception debugbreak called in debug mode!
				if (audio->termination_mark.AudioBytes < 8)
				{
					// XAudio2: AudioBytes (4) not aligned to the audio format's block size (8)
					return;
				}
				else
				{
					hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&audio->termination_mark);
					assert(SUCCEEDED(hr));
				}
				#else
				hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&audio->termination_mark);
				assert(SUCCEEDED(hr));
				#endif
			}
			hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&instanceinternal->buffer); // resubmit
			assert(SUCCEEDED(hr));
		}
	}
	void StopLooping(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr = instanceinternal->sourceVoice->Stop(); // preserves cursor position
			assert(SUCCEEDED(hr));
		}
	}
	void SetVolume(float volume, SoundInstance* instance)
	{
		if (instance == nullptr || !instance->IsValid())
		{
			HRESULT hr = audio->masteringVoice->SetVolume(volume);
			assert(SUCCEEDED(hr));
		}
		else
		{
			auto instanceinternal = to_internal(instance);
			HRESULT hr = instanceinternal->sourceVoice->SetVolume(volume);
			assert(SUCCEEDED(hr));
		}
	}
	float GetVolume(const SoundInstance* instance)
	{
		float volume = 0;
		if (instance == nullptr || !instance->IsValid())
		{
			audio->masteringVoice->GetVolume(&volume);
		}
		else
		{
			auto instanceinternal = to_internal(instance);
			instanceinternal->sourceVoice->GetVolume(&volume);
		}
		return volume;
	}
	void ExitLoop(SoundInstance* instance)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);
			if (instanceinternal->buffer.LoopCount == 0)
				return;
			HRESULT hr = instanceinternal->sourceVoice->ExitLoop();
			assert(SUCCEEDED(hr));

			if (instanceinternal->pCallback && instanceinternal->pCallback->IsFinished())
			{
				instanceinternal->buffer.LoopCount = 0;
				hr = instanceinternal->sourceVoice->SubmitSourceBuffer(&instanceinternal->buffer);
				assert(SUCCEEDED(hr));
			}
		}
	}

	void SetSubmixVolume(SUBMIX_TYPE type, float volume)
	{
		HRESULT hr = audio->submixVoices[type]->SetVolume(volume);
		assert(SUCCEEDED(hr));
	}
	float GetSubmixVolume(SUBMIX_TYPE type)
	{
		float volume;
		audio->submixVoices[type]->GetVolume(&volume);
		return volume;
	}

	void Update3D(SoundInstance* instance, const SoundInstance3D& instance3D, float CurveDistanceScaler)
	{
		if (instance != nullptr && instance->IsValid())
		{
			auto instanceinternal = to_internal(instance);

			X3DAUDIO_LISTENER listener = {};
			listener.Position = instance3D.listenerPos;
			listener.OrientFront = instance3D.listenerFront;
			listener.OrientTop = instance3D.listenerUp;
			listener.Velocity = instance3D.listenerVelocity;

			X3DAUDIO_EMITTER emitter = {};
			emitter.Position = instance3D.emitterPos;

			//PE: Slight difference in volume (left to right) , set emitter's orientation to match the listener's
			emitter.OrientFront = instance3D.listenerFront; //PE: Make emitter face same way as listener
			emitter.OrientTop = instance3D.listenerUp;   //PE: Make emitter's up match listener's up

			//emitter.OrientFront = instance3D.emitterFront;
			//emitter.OrientTop = instance3D.emitterUp;
			emitter.Velocity = instance3D.emitterVelocity;
			//PE: True point sound settings for omnidirectional.
			emitter.InnerRadius = 0;
			emitter.InnerRadiusAngle = X3DAUDIO_2PI;

			//emitter.InnerRadius = instance3D.emitterRadius;
			//emitter.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;
			emitter.ChannelCount = instanceinternal->voiceDetails.InputChannels;
			emitter.pChannelAzimuths = instanceinternal->channelAzimuths.data();
			emitter.ChannelRadius = 0.1f;
			//emitter.CurveDistanceScaler = 1;
			emitter.CurveDistanceScaler = CurveDistanceScaler;// 180; //150,200,350, 400; //PE: GGREDUCED
			emitter.DopplerScaler = 1;

			UINT32 flags = 0;
			flags |= X3DAUDIO_CALCULATE_MATRIX;
			flags |= X3DAUDIO_CALCULATE_LPF_DIRECT;
			flags |= X3DAUDIO_CALCULATE_REVERB;
			flags |= X3DAUDIO_CALCULATE_LPF_REVERB;
			flags |= X3DAUDIO_CALCULATE_DOPPLER;
			//flags |= X3DAUDIO_CALCULATE_DELAY;
			//flags |= X3DAUDIO_CALCULATE_EMITTER_ANGLE;
			//flags |= X3DAUDIO_CALCULATE_ZEROCENTER;
			//flags |= X3DAUDIO_CALCULATE_REDIRECT_TO_LFE;

			X3DAUDIO_DSP_SETTINGS settings = {};
			settings.SrcChannelCount = instanceinternal->voiceDetails.InputChannels;
			settings.DstChannelCount = audio->masteringVoiceDetails.InputChannels;
			settings.pMatrixCoefficients = instanceinternal->outputMatrix.data();

			X3DAudioCalculate(audio->audio3D, &listener, &emitter, flags, &settings);

			HRESULT hr;

			hr = instanceinternal->sourceVoice->SetFrequencyRatio(settings.DopplerFactor);
			assert(SUCCEEDED(hr));

			hr = instanceinternal->sourceVoice->SetOutputMatrix(
				audio->submixVoices[instance->type],
				settings.SrcChannelCount, 
				settings.DstChannelCount, 
				settings.pMatrixCoefficients
			);
			assert(SUCCEEDED(hr));
			
			XAUDIO2_FILTER_PARAMETERS FilterParametersDirect = { LowPassFilter, 2.0f * sinf(X3DAUDIO_PI / 6.0f * settings.LPFDirectCoefficient), 1.0f };
			hr = instanceinternal->sourceVoice->SetOutputFilterParameters(audio->submixVoices[instance->type], &FilterParametersDirect);
			assert(SUCCEEDED(hr));

			if (instance->IsEnableReverb() && instanceinternal->audio->reverbSubmix != nullptr)
			{
				hr = instanceinternal->sourceVoice->SetOutputMatrix(audio->reverbSubmix, settings.SrcChannelCount, 1, &settings.ReverbLevel);
				assert(SUCCEEDED(hr));
				XAUDIO2_FILTER_PARAMETERS FilterParametersReverb = { LowPassFilter, 2.0f * sinf(X3DAUDIO_PI / 6.0f * settings.LPFReverbCoefficient), 1.0f };
				hr = instanceinternal->sourceVoice->SetOutputFilterParameters(audio->reverbSubmix, &FilterParametersReverb);
				assert(SUCCEEDED(hr));
			}
		}
	}

	void SetReverb(REVERB_PRESET preset)
	{
		if (audio->reverbSubmix != nullptr)
		{
			XAUDIO2FX_REVERB_PARAMETERS native;
			ReverbConvertI3DL2ToNative(&reverbPresets[preset], &native);
			HRESULT hr = audio->reverbSubmix->SetEffectParameters(0, &native, sizeof(native));
			assert(SUCCEEDED(hr));
		}
	}
}

#else

namespace wiAudio
{
	void Initialize() {}

	bool CreateSound(const std::string& filename, Sound* sound) { return false; }
	bool CreateSound(const std::vector<uint8_t>& data, Sound* sound) { return false; }
	bool CreateSound(const uint8_t* data, size_t size, Sound* sound) { return false; }
	bool CreateSoundInstance(const Sound* sound, SoundInstance* instance) { return false; }

	void Play(SoundInstance* instance) {}
	void Pause(SoundInstance* instance) {}
	void Stop(SoundInstance* instance) {}
	void SetVolume(float volume, SoundInstance* instance) {}
	float GetVolume(const SoundInstance* instance) { return 0; }
	void ExitLoop(SoundInstance* instance) {}

	void SetSubmixVolume(SUBMIX_TYPE type, float volume) {}
	float GetSubmixVolume(SUBMIX_TYPE type) { return 0; }

	void Update3D(SoundInstance* instance, const SoundInstance3D& instance3D, float CurveDistanceScaler) {}

	void SetReverb(REVERB_PRESET preset) {}
}

#endif // _WIN32
