/*
 *  espeakup - interface which allows speakup to use espeak
 *
 *  Copyright (C) 2008 William Hubbs
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espeakup.h"

/* default voice settings */
const int defaultFrequency = 5;
const int defaultPitch = 5;
const int defaultRate = 5;
const int defaultVolume = 5;

char *defaultVoice = NULL;

pthread_cond_t runner_awake = PTHREAD_COND_INITIALIZER;
pthread_cond_t stop_acknowledged = PTHREAD_COND_INITIALIZER;
pthread_mutex_t queue_guard = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stop_guard = PTHREAD_MUTEX_INITIALIZER;

volatile int runner_must_stop = 0;
static struct queue_entry_t *first = NULL;
static struct queue_entry_t *last = NULL;

void queue_add(struct queue_entry_t *entry)
{
	pthread_mutex_lock(&queue_guard);
	assert(entry);
	entry->next = NULL;
	if (!last)
		last = entry;
	if (!first) {
		first = entry;
	} else {
		first->next = entry;
		first = first->next;
	}
	pthread_mutex_unlock(&queue_guard);
	pthread_cond_signal(&runner_awake);
}

static void free_entry(struct queue_entry_t *entry)
{
	if (entry->cmd == CMD_SPEAK_TEXT)
		free(entry->buf);
	free(entry);
}

/* Remove and return the entry at the head of the queue.
 * Return NULL if queue is empty. */

void queue_remove(void)
{
	struct queue_entry_t *temp;

	if (last) {
		temp = last;
		last = temp->next;
		if (!last)
			first = last;
		free_entry(temp);
	}
}

void queue_clear(void)
{
	struct queue_entry_t *temp;

	while (last) {
		temp = last->next;
		queue_remove();
	}
	/* We aren't adding data to the queue, so no need to signal. */
}

static void queue_process_entry(struct synth_t *s)
{
	espeak_ERROR error;
	struct queue_entry_t *current = last;

	pthread_mutex_unlock(&queue_guard);	/* So "reader" can go. */
	if (current) {
		switch (current->cmd) {
		case CMD_SET_FREQUENCY:
			error = set_frequency(s, current->value, current->adjust);
			break;
		case CMD_SET_PITCH:
			error = set_pitch(s, current->value, current->adjust);
			break;
		case CMD_SET_PUNCTUATION:
			error = set_punctuation(s, current->value, current->adjust);
			break;
		case CMD_SET_RATE:
			error = set_rate(s, current->value, current->adjust);
			break;
		case CMD_SET_VOICE:
			break;
		case CMD_SET_VOLUME:
			error = set_volume(s, current->value, current->adjust);
			break;
		case CMD_SPEAK_TEXT:
			s->buf = current->buf;
			s->len = current->len;
			error = speak_text(s);
			break;
		default:
			break;
		}

		pthread_mutex_lock(&queue_guard);
		if (error == EE_OK)
			queue_remove();
		pthread_mutex_unlock(&queue_guard);
	}
}

/*
 * Tell the runner to stop speech and clear its queue.
 */
void stop_runner(void)
{
	pthread_mutex_lock(&stop_guard);
	pthread_mutex_lock(&queue_guard);
	runner_must_stop = 1;
	pthread_mutex_unlock(&queue_guard);
	pthread_cond_signal(&runner_awake);	/* Wake runner, if necessary. */
	pthread_cond_wait(&stop_acknowledged, &stop_guard);
	pthread_mutex_unlock(&stop_guard);
}

/* espeak_thread is the "main" function of our secondary (queue-processing)
 * thread.
 * First, lock queue_guard, because it needs to be locked when we call
 * pthread_cond_wait on the runner_awake condition variable.
 * Next, enter an infinite loop.
 * The wait call also unlocks queue_guard, so that the other thread can
 * manipulate the queue.
 * When runner_awake is signaled, the pthread_cond_wait call re-locks
 * queue_guard, and the "queue processor" thread has access to the queue.
 * While there is an entry in the queue, call queue_process_entry.
 * queue_process_entry unlocks queue_guard after removing an item from the
 * queue, so that the main thread doesn't have to wait for us to finish
 * processing the entry.  So re-lock queue_guard after each call to
 * queue_process_entry.
 *
 * The main thread can add items to the queue in exactly two situations:
 * 1. We are waiting on runner_awake, or
 * 2. We are processing an entry that has just been removed from the queue.
*/

void *espeak_thread(void *arg)
{
	struct synth_t *synth = (struct synth_t *) arg;
	int rate;

	/* initialize espeak */
	select_audio_mode();
	rate = espeak_Initialize(audio_mode, 0, NULL, 0);
	if (rate < 0) {
		fprintf(stderr, "Unable to initialize espeak.\n");
		should_run = 0;
	}

	if (init_audio((unsigned int) rate) < 0) {
		should_run = 0;
	}

	/* Setup initial voice parameters */
	if (defaultVoice) {
		set_voice(&s, defaultVoice);
		free(defaultVoice);
		defaultVoice = NULL;
	}
	set_frequency(&s, defaultFrequency, ADJ_SET);
	set_pitch(&s, defaultPitch, ADJ_SET);
	set_rate(&s, defaultRate, ADJ_SET);
	set_volume(&s, defaultVolume, ADJ_SET);
	espeak_SetParameter(espeakCAPITALS, 0, 0);

	pthread_mutex_lock(&queue_guard);
	while (should_run) {
		pthread_cond_wait(&runner_awake, &queue_guard);

		while (should_run && last && !runner_must_stop) {
			queue_process_entry(synth);
			pthread_mutex_lock(&queue_guard);
		}

		if (runner_must_stop) {
			pthread_mutex_lock(&stop_guard);
			queue_clear();
			stop_speech();
			runner_must_stop = 0;
			pthread_mutex_unlock(&stop_guard);
			pthread_cond_signal(&stop_acknowledged);
		}
	}

	espeak_Terminate();
	return NULL;
}
