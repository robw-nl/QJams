#ifndef AUDIO_NLE_H
#define AUDIO_NLE_H

#include <stddef.h>
#include <stdint.h>

void internal_backup_masked_tracks(uint32_t mask);
void apply_nle_cut_math(int track_idx, size_t start, size_t end);
void apply_nle_clear_math(int track_idx, size_t start, size_t end);

void copy_selection(void);
void cut_selection(void);
void paste_selection(void);
void global_paste_selection(void);
void delete_selection(void);
void global_ripple_delete_selection(void);
void apply_smart_fade_selection(void);
void apply_blend_fade_selection(void);
void apply_gain_selection(float multiplier);
void undo_selection(void);
void redo_selection(void);
void crop_canvas(void);
float *render_mixdown_region(size_t start, size_t end);

#endif // AUDIO_NLE_H
