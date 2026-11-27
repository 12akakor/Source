/*
 * Copyright 2005, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Application.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>


extern const char *__progname;
static const char *sProgramName = __progname;


// without parameters to get the actual mute status
int
handleMute(BParameter *muteParameter, int mute = 2, bool toggle = false) {
	int32 muted = 0;
	bigtime_t lastChange = 0;
	size_t size = sizeof(int32);
	muteParameter->GetValue(&muted, &size, &lastChange);
	if (toggle)
		mute = 1 - muted;
	if (muted != mute && mute < 2)
		muteParameter->SetValue(&mute, sizeof(int32), system_time());
	return muted;
}


// Sets the mutes of the recording channels: the groups that also hold the
// input source selector (B_INPUT_MUX) of a converter. A mute of -1 (toggle)
// is decided by the first channel. Returns the number of channels set.
static int32
setInputMutes(BParameterGroup *group, int &mute)
{
	BParameter *muteParameter = NULL;
	bool recording = false;
	for (int32 i = 0; i < group->CountParameters(); i++) {
		BParameter *parameter = group->ParameterAt(i);
		if (!strcmp(parameter->Kind(), B_MUTE))
			muteParameter = parameter;
		else if (!strcmp(parameter->Kind(), B_INPUT_MUX))
			recording = true;
	}
	int32 count = 0;
	if (recording && muteParameter != NULL) {
		if (mute < 0)
			mute = 1 - handleMute(muteParameter);
		handleMute(muteParameter, mute);
		count++;
	}

	for (int32 i = 0; i < group->CountGroups(); i++)
		count += setInputMutes(group->GroupAt(i), mute);
	return count;
}


// Mutes (1), unmutes (0) or toggles (-1) the microphone: every recording
// channel of the default audio input.
static int
handleMicrophone(BMediaRoster *roster, int mute)
{
	media_node input;
	status_t status = roster->GetAudioInput(&input);
	if (status != B_OK) {
		fprintf(stderr, "%s: cannot get audio input: %s\n", sProgramName,
			strerror(status));
		return 1;
	}

	BParameterWeb *web;
	status = roster->GetParameterWebFor(input, &web);
	roster->ReleaseNode(input);
	if (status != B_OK) {
		fprintf(stderr, "%s: cannot get parameter web for audio input: %s\n",
			sProgramName, strerror(status));
		return 1;
	}

	int32 count = 0;
	for (int32 i = 0; i < web->CountGroups(); i++)
		count += setInputMutes(web->GroupAt(i), mute);
	if (count == 0) {
		fprintf(stderr, "%s: the audio input has no mute control\n",
			sProgramName);
		delete web;
		return 1;
	}

	printf("Microphone %s\n", mute ? "muted" : "unmuted");
	delete web;
	return 0;
}


int
main(int argc, char **argv)
{
	BApplication app("application/x-vnd.haiku.setvolume");

	BMediaRoster *roster = BMediaRoster::Roster();
	if (roster == NULL) {
		fprintf(stderr, "%s: media roster not available\n", sProgramName);
		return 1;
	}

	if (argc > 1) {
		if (strcmp(argv[1], "-M") == 0 || strcmp(argv[1], "--mic-mute") == 0)
			return handleMicrophone(roster, 1);
		if (strcmp(argv[1], "-U") == 0 || strcmp(argv[1], "--mic-unmute") == 0)
			return handleMicrophone(roster, 0);
		if (strcmp(argv[1], "-T") == 0
			|| strcmp(argv[1], "--mic-togglemute") == 0) {
			return handleMicrophone(roster, -1);
		}
	}

	media_node mixer;
	status_t status = roster->GetAudioMixer(&mixer);
	if (status != B_OK) {
		fprintf(stderr, "%s: cannot get audio mixer: %s\n", sProgramName, strerror(status));
		return 1;
	}

	BParameterWeb *web;
	status = roster->GetParameterWebFor(mixer, &web);

	roster->ReleaseNode(mixer);
		// the web is all we need :-)

	if (status != B_OK) {
		fprintf(stderr, "%s: cannot get parameter web for audio mixer: %s\n",
			sProgramName, strerror(status));
		return 1;
	}

	BContinuousParameter *gain = NULL;
	BParameter *mute = NULL;

	BParameter *parameter;
	for (int32 index = 0; (parameter = web->ParameterAt(index)) != NULL; index++) {
		// assume the mute preceeding master gain control
		if (!strcmp(parameter->Kind(), B_MUTE))
			mute = parameter;

		if (!strcmp(parameter->Kind(), B_MASTER_GAIN)) {
			gain = dynamic_cast<BContinuousParameter *>(parameter);
			break;
		}
	}

	if (gain == NULL) {
		fprintf(stderr, "%s: could not found master gain!\n", sProgramName);
		delete web;
		return 1;
	}

	float volume = 0.0;

	if (argc > 1) {
		if (strcmp(argv[1], "-m") == 0 || strcmp(argv[1], "--mute") == 0) {
			handleMute(mute, 1);
			printf("Muted\n");
			return 0;
		} else if (strcmp(argv[1], "-u") == 0 || strcmp(argv[1], "--unmute") == 0) {
			handleMute(mute, 0);
			printf("Unmuted\n");
			return 0;
		} else if (strcmp(argv[1], "-t") == 0 || strcmp(argv[1], "--togglemute") == 0) {
			int32 muted = handleMute(mute, 0, true);
			printf("%s\n", muted ? "Muted" : "Unmuted");
			return 0;
		} else {
			if (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "-d") == 0
				|| strcmp(argv[1], "--increase") == 0 || strcmp(argv[1], "--decrease") == 0) {
				bigtime_t when;
				size_t size = sizeof(volume);
				gain->GetValue(&volume, &size, &when);
				size_t step = 3;
				if (argc > 2)
					step = atoi(argv[2]);
				if (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "--increase") == 0) {
					// make sure it's unmuted
					int muted = handleMute(mute, 0);
					if (muted == 0 || volume <= gain->MinValue())
						volume += step;
				} else if (handleMute(mute) == 0) {
					// not already muted
					volume -= step;
					// make sure it's muted
					if (volume <= gain->MinValue())
						handleMute(mute, 1);
				}
			} else {
				char *end;
				volume = strtod(argv[1], &end);
				if (end == argv[1]) {
					fprintf(stderr,
						"Usage: %s <volume> | [options]\n"
						"Sets the system volume to the specified value in dB.\n"
						"Alternatively there are these options:\n"
						"  -m  --mute\n"
						"  -u  --unmute\n"
						"  -t  --togglemute\ttoggles muting\n"
						"  -i  --increase x\tincreases volume by x dB\n"
						"  -d  --decrease x\tdecreases volume by x dB\n"
						"\t\t\tx defaults to 3 if not supplied\n"
						"  -M  --mic-mute\tmutes the microphone\n"
						"  -U  --mic-unmute\n"
						"  -T  --mic-togglemute\ttoggles muting the microphone\n",
						sProgramName);
					exit(1);
				}
			}

			// make sure our parameter is in range
			if (volume > gain->MaxValue())
				volume = gain->MaxValue();
			else if (volume < gain->MinValue())
				volume = gain->MinValue();

			gain->SetValue(&volume, sizeof(volume), system_time());
		}
	}
	bigtime_t when;
	size_t size = sizeof(volume);
	gain->GetValue(&volume, &size, &when);

	printf("Current volume: %g (min = %g, max = %g)\n", volume, gain->MinValue(), gain->MaxValue());

	delete web;
	return 0;
}

