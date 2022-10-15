/*
 * MIT License
 *
 * Copyright (c) 2022 Andreas Nebinger
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include "alarm_face.h"
#include "watch.h"
#include "watch_utility.h"

/*
    Implements 10 alarm slots on the sensor watch

    Usage:
    - In normal mode, the alarm button cycles through all 10 alarms. 
    - Long pressing the alarm button in normal mode toggles the corresponding alarm on or off.
    - Pressing the light button enters setting mode and cycles through the settings of each alarm.
    - In setting mode an alarm slot is selected by pressing the alarm button when the slot number 
      in the upper right corner is blinking.
    - For each alarm slot, you can select the day. These are the day modes:
        - ED = the alarm rings every day
        - 1t = the alarm fires only one time and is erased afterwards
        - MF = the alarm fires Mondays to Fridays (yes, I am aware of the ambiguity of the abbreviation  ;-)
        - WN = the alarm fires on weekends (Sa/Su)
        - MO to SU = the alarm fires only on the given day of week
    - You can fast jump (kinda) through hour or minute setting via long press of the alarm button.
    - You can select the tone in which the alarm is played. (Three pitch levels available)
    - You can select how many "beep rounds" are played for each alarm (1 to 9).
    - The simple watch face indicates any alarm set by showing the bell indicator.
*/

static const char _dow_strings[ALARM_DAY_STATES + 1][3] ={"AL", "MO", "TU", "WE", "TH", "FR", "SA", "SO", "ED", "1t", "MF", "WN"};
static const uint8_t _blink_idx[ALARM_SETTING_STATES] = {2, 0, 4, 6, 8, 9};
static const uint8_t _blink_idx2[ALARM_SETTING_STATES] = {3, 1, 5, 7, 8, 9};
static const BuzzerNote _buzzer_notes[3] = {BUZZER_NOTE_B6, BUZZER_NOTE_C8, BUZZER_NOTE_A8};
static const uint8_t _buzzer_segdata[3][2] = {{0, 3}, {1, 3}, {2, 2}};

static uint8_t _get_weekday_idx(watch_date_time date_time) {
    date_time.unit.year += 20;
    if (date_time.unit.month <= 2) {
        date_time.unit.month += 12;
        date_time.unit.year--;
    }
    return (date_time.unit.day + 13 * (date_time.unit.month + 1) / 5 + date_time.unit.year + date_time.unit.year / 4 + 525 - 2) % 7;
}

static void _alarm_face_draw(movement_settings_t *settings, alarm_state_t *state, uint8_t subsecond) {
    char buf[12];

    uint8_t i = 0;
    if (state->is_setting) {
        // display the actual day indicating string for the current alarm
        i = state->alarm[state->alarm_idx].day + 1;
    }
    //handle am/pm for hour display
    uint8_t h = state->alarm[state->alarm_idx].hour;
    if (!settings->bit.clock_mode_24h) {
        if (h > 12) {
            watch_set_indicator(WATCH_INDICATOR_PM);
            h -= 12;
        } else {
            watch_clear_indicator(WATCH_INDICATOR_PM);
        }
    }
    sprintf(buf, "%s%2d%2d%02d %1d",
        _dow_strings[i],
        (state->alarm_idx + 1),
        h,
        state->alarm[state->alarm_idx].minute,
        state->alarm[state->alarm_idx].beeps + 1);
    // don't show beep rounds in normal mode to avoid user confusion
    if (!state->is_setting) 
        buf[_blink_idx[5]] = buf[_blink_idx2[5]] = ' ';
    // blink items if in settings mode
    if (state->is_setting && subsecond % 2) {
        buf[_blink_idx[state->setting_state]] = buf[_blink_idx2[state->setting_state]] = ' ';
    }
    watch_display_string(buf, 0);
    
    // draw pitch level indicator
    if (state->is_setting && ((subsecond % 2) == 0 || (state->setting_state != 4))) {
        for (i = 0; i <= state->alarm[state->alarm_idx].pitch && i < 3; i++)
            watch_set_pixel(_buzzer_segdata[i][0], _buzzer_segdata[i][1]);
    }
    // set bell indicator
    if (state->alarm[state->alarm_idx].enabled)
        watch_set_indicator(WATCH_INDICATOR_BELL);
    else
        watch_clear_indicator(WATCH_INDICATOR_BELL);

}

static void _alarm_resume_setting(movement_settings_t *settings, alarm_state_t *state, uint8_t subsecond) {
    state->is_setting = false;
    movement_request_tick_frequency(1);
    _alarm_face_draw(settings, state, subsecond);
}

void alarm_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr) {
    (void) settings;
    (void) watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(alarm_state_t));
        alarm_state_t *state = (alarm_state_t *)*context_ptr;
        memset(*context_ptr, 0, sizeof(alarm_state_t));
        // initialize the default alarm values
        for (uint8_t i = 0; i < ALARM_ALARMS; i++) {
            state->alarm[i].day = ALARM_DAY_EACH_DAY;
            state->alarm[i].beeps = 5;
            state->alarm[i].pitch = 1;
        }
        state->alarm_handled_minute = -1;
    }
}

void alarm_face_activate(movement_settings_t *settings, void *context) {
    (void) settings;
    (void) context;
    watch_display_string("  ", 8);
    watch_clear_indicator(WATCH_INDICATOR_LAP); // may be unnecessary, but who knows
    watch_set_colon();
}

void alarm_face_resign(movement_settings_t *settings, void *context) {
    alarm_state_t *state = (alarm_state_t *)context;
    state->is_setting = false;
    // save indication for active alarms to movement settings
    bool active_alarms = false;
    for (uint8_t i = 0; i < ALARM_ALARMS; i++) {
        if (state->alarm[i].enabled) {
            active_alarms = true;
            break;
        }
    }
    settings->bit.alarm_enabled = active_alarms;
    watch_set_led_off();
    watch_store_backup_data(settings->reg, 0);
}

bool alarm_face_wants_background_task(movement_settings_t *settings, void *context) {
    (void) settings;
    alarm_state_t *state = (alarm_state_t *)context;
    watch_date_time now = watch_rtc_get_date_time();
    // just a failsafe: never fire more than one alarm within a minute
    if (state->alarm_handled_minute == now.unit.minute) return false;
    state->alarm_handled_minute = now.unit.minute;
    // check the rest
    for (uint8_t i = 0; i < ALARM_ALARMS; i++) {
        if (state->alarm[i].enabled) {
            if (state->alarm[i].minute == now.unit.minute) {
                if (state->alarm[i].hour == now.unit.hour) {
                    state->alarm_playing_idx = i;
                    if (state->alarm[i].day == ALARM_DAY_EACH_DAY) return true;
                    if (state->alarm[i].day == ALARM_DAY_ONE_TIME) {
                        // erase this alarm
                        state->alarm[i].day = ALARM_DAY_EACH_DAY;
                        state->alarm[i].minute = state->alarm[i].hour = 0;
                        state->alarm[i].enabled = false;
                        return true;
                    }
                    uint8_t weekday_idx = _get_weekday_idx(now);
                    if (state->alarm[i].day == weekday_idx) return true;
                    if (state->alarm[i].day == ALARM_DAY_WORKDAY && weekday_idx < 5) return true;
                    if (state->alarm[i].day == ALARM_DAY_WEEKEND && weekday_idx >= 5) return true;
                }
            }
        }
    }
    state->alarm_handled_minute = -1;
    return false;
}

bool alarm_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    (void) settings;
    alarm_state_t *state = (alarm_state_t *)context;

    switch (event.event_type) {
    case EVENT_ACTIVATE:
    case EVENT_TICK:
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_LIGHT_BUTTON_DOWN:
        break;
    case EVENT_LIGHT_BUTTON_UP:
        if (!state->is_setting) movement_illuminate_led();
        if (!state->is_setting) {
            state->is_setting = true;
            state->setting_state = 0;
            movement_request_tick_frequency(4);
            _alarm_face_draw(settings, state, event.subsecond);
            break;
        }
        state->setting_state += 1;
        if (state->setting_state >= ALARM_SETTING_STATES) {
            // we have done a full settings cycle, so resume to normal
            _alarm_resume_setting(settings, state, event.subsecond);
        }
        break;
    case EVENT_LIGHT_LONG_PRESS:
        if (state->is_setting) {
            _alarm_resume_setting(settings, state, event.subsecond);
        }
        break;
    case EVENT_ALARM_BUTTON_UP:
        if (!state->is_setting) {
            // cycle through the alarms
            state->alarm_idx = (state->alarm_idx + 1) % (ALARM_ALARMS);
        } else {
            // handle the settings behaviour
            switch (state->setting_state) {
            case 0:
                // alarm selection
                state->alarm_idx = (state->alarm_idx + 1) % (ALARM_ALARMS);
                break;
            case 1:
                // day selection
                state->alarm[state->alarm_idx].day = (state->alarm[state->alarm_idx].day + 1) % (ALARM_DAY_STATES);
                break;
            case 2:
                // hour selection
                state->alarm[state->alarm_idx].hour = (state->alarm[state->alarm_idx].hour + 1) % 24;
                break;
            case 3:
                // minute selection
                state->alarm[state->alarm_idx].minute = (state->alarm[state->alarm_idx].minute + 1) % 60;
                break;
            case 4:
                // pitch level
                state->alarm[state->alarm_idx].pitch = (state->alarm[state->alarm_idx].pitch + 1) % 3;
                // play sound to indicate user what we are doing
                watch_buzzer_play_note(_buzzer_notes[state->alarm[state->alarm_idx].pitch], 50);
                watch_buzzer_play_note(BUZZER_NOTE_REST, 50);
                watch_buzzer_play_note(_buzzer_notes[state->alarm[state->alarm_idx].pitch], 75);
                break;
            case 5:
                // number of beeping rounds selection
                state->alarm[state->alarm_idx].beeps = (state->alarm[state->alarm_idx].beeps + 1) % (ALARM_MAX_BEEP_ROUNDS);
                break;
            default:
                break;
            }
            // auto enable an alarm if user sets anything
            if (state->setting_state > 0) state->alarm[state->alarm_idx].enabled = true;
        }
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_ALARM_LONG_PRESS:
        if (!state->is_setting) {
            // toggle the enabled flag for current alarm
            state->alarm[state->alarm_idx].enabled ^= 1;
        } else {
            // handle the long press settings behaviour
            switch (state->setting_state) {
            case 2:
                // hour selection
                state->alarm[state->alarm_idx].hour = ((state->alarm[state->alarm_idx].hour / 12) * 12 + 12) % 24;
                break;
            case 3:
                // minute selection
                state->alarm[state->alarm_idx].minute = ((state->alarm[state->alarm_idx].minute / 15) * 15 + 15) % 60;
                break;
            default:
                break;
            }
            // auto enable an alarm if user sets anything
            if (state->setting_state > 0) state->alarm[state->alarm_idx].enabled = true;
        }
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_BACKGROUND_TASK:
        // play alarm
        movement_play_alarm_beeps(state->alarm[state->alarm_playing_idx].beeps + 1, _buzzer_notes[state->alarm[state->alarm_playing_idx].pitch]);
        break;
    case EVENT_MODE_BUTTON_UP:
        movement_move_to_next_face();
        break;
    case EVENT_TIMEOUT:
        movement_move_to_face(0);
        break;
    case EVENT_LOW_ENERGY_UPDATE:
    default:
      break;
    }

    return true;
}