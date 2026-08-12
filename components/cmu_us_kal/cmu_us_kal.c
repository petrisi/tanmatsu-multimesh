/*************************************************************************/
/*                                                                       */
/*                  Language Technologies Institute                      */
/*                     Carnegie Mellon University                        */
/*                      Copyright (c) 1999-2011                          */
/*                        All Rights Reserved.                           */
/*                                                                       */
/*  Permission is hereby granted, free of charge, to use and distribute  */
/*  this software and its documentation without restriction, including   */
/*  without limitation the rights to use, copy, modify, merge, publish,  */
/*  distribute, sublicense, and/or sell copies of this work, and to      */
/*  permit persons to whom this work is furnished to do so, subject to   */
/*  the following conditions:                                            */
/*   1. The code must retain the above copyright notice, this list of    */
/*      conditions and the following disclaimer.                         */
/*   2. Any modifications must be clearly marked as such.                */
/*   3. Original authors' names are not deleted.                         */
/*   4. The authors' names are not used to endorse or promote products   */
/*      derived from this software without specific prior written        */
/*      permission.                                                      */
/*                                                                       */
/*  CARNEGIE MELLON UNIVERSITY AND THE CONTRIBUTORS TO THIS WORK         */
/*  DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING      */
/*  ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT   */
/*  SHALL CARNEGIE MELLON UNIVERSITY NOR THE CONTRIBUTORS BE LIABLE      */
/*  FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES    */
/*  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN   */
/*  AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,          */
/*  ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF       */
/*  THIS SOFTWARE.                                                       */
/*                                                                       */
/*************************************************************************/
/*             Author:  Alan W Black (awb@cs.cmu.edu)                    */
/*               Date:  November 2011                                    */
/*************************************************************************/
/*                                                                       */
/*  Kal diphone, rebuilt with g721 encoded residuals to save 500K        */
/*                                                                       */
/*************************************************************************/

#include "flite.h"
#include "cst_diphone.h"
#include "usenglish.h"
#include "cmu_lex.h"

static cst_utterance *cmu_us_kal_postlex(cst_utterance *u);
extern cst_diphone_db cmu_us_kal_db;
extern cst_sts_list cmu_us_kal_sts;

unsigned short *cmu_us_kal_lpc = NULL;
unsigned char *cmu_us_kal_res = NULL;
unsigned int *cmu_us_kal_resi = NULL;
unsigned char *cmu_us_kal_ressize = NULL;

#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static void load_bin_file(const char *path, void **ptr, int caps) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE("flite_voice", "Failed to open %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    *ptr = heap_caps_malloc(size, caps);
    if (!*ptr) {
        ESP_LOGE("flite_voice", "Failed to allocate %d bytes for %s in PSRAM", (int)size, path);
        fclose(f);
        return;
    }
    size_t read = fread(*ptr, 1, size, f);
    if (read != size) {
        ESP_LOGE("flite_voice", "Failed to read full %s (read %d, expected %d)", path, (int)read, (int)size);
    }
    fclose(f);
    ESP_LOGI("flite_voice", "Loaded %s (%d bytes) to %p", path, (int)size, *ptr);
}

int cmu_us_kal_load_voice(void) {
    if (cmu_us_kal_lpc && cmu_us_kal_res && cmu_us_kal_resi && cmu_us_kal_ressize) return 0; // already loaded
    
    load_bin_file("/locfd/multimesh/kal_lpc.bin", (void**)&cmu_us_kal_lpc, MALLOC_CAP_SPIRAM);
    load_bin_file("/locfd/multimesh/kal_res.bin", (void**)&cmu_us_kal_res, MALLOC_CAP_SPIRAM);
    load_bin_file("/locfd/multimesh/kal_resi.bin", (void**)&cmu_us_kal_resi, MALLOC_CAP_SPIRAM);
    load_bin_file("/locfd/multimesh/kal_ressize.bin", (void**)&cmu_us_kal_ressize, MALLOC_CAP_SPIRAM);
    
    if (!cmu_us_kal_lpc || !cmu_us_kal_res || !cmu_us_kal_resi || !cmu_us_kal_ressize) {
        ESP_LOGE("flite_voice", "Failed to load voice files from SD card");
        return -1;
    }
    
    cmu_us_kal_sts.frames = cmu_us_kal_lpc;
    cmu_us_kal_sts.residuals = cmu_us_kal_res;
    cmu_us_kal_sts.resoffs = cmu_us_kal_resi;
    cmu_us_kal_sts.ressizes = cmu_us_kal_ressize;
    return 0;
}

cst_voice *cmu_us_kal_diphone = NULL;

cst_voice *register_cmu_us_kal(const char *voxdir)
{
    cst_voice *v;
    cst_lexicon *lex;

    if (cmu_us_kal_diphone)
        return cmu_us_kal_diphone;  /* Already registered */
    
    v  = new_voice();
    v->name = "kal";

    if (cmu_us_kal_load_voice() != 0) {
        delete_voice(v);
        return NULL;
    }

    /* Set up basic values for synthesizing with this voice */
    usenglish_init(v);
    flite_feat_set_string(v->features,"name","cmu_us_kal_diphone");

    /* Lexicon */
    lex = cmu_lex_init();
    flite_feat_set(v->features,"lexicon",lexicon_val(lex));

    /* Intonation */
    flite_feat_set_float(v->features,"int_f0_target_mean",95.0);
    flite_feat_set_float(v->features,"int_f0_target_stddev",11.0);

    flite_feat_set_float(v->features,"duration_stretch",1.1); 

    /* Post lexical rules */
    flite_feat_set(v->features,"postlex_func",uttfunc_val(&cmu_us_kal_postlex));

    /* Waveform synthesis: diphone_synth */
    flite_feat_set(v->features,"wave_synth_func",uttfunc_val(&diphone_synth));
    flite_feat_set(v->features,"diphone_db",diphone_db_val(&cmu_us_kal_db));
    flite_feat_set_int(v->features,"sample_rate",cmu_us_kal_db.sts->sample_rate);
    flite_feat_set_string(v->features,"resynth_type","fixed");
    flite_feat_set_string(v->features,"join_type","modified_lpc");

    cmu_us_kal_diphone = v;

    return cmu_us_kal_diphone;
}

void unregister_cmu_us_kal(cst_voice *v)
{
    if (v != cmu_us_kal_diphone)
	return;
    delete_voice(v);
    cmu_us_kal_diphone = NULL;
}

static void fix_ah(cst_utterance *u)
{
    /* This should really be done in the index itself */
    const cst_item *s;

    for (s=relation_head(utt_relation(u,"Segment")); s; s=item_next(s))
	if (cst_streq(item_feat_string(s,"name"),"ah"))
	    item_set_string(s,"name","aa");
}

static cst_utterance *cmu_us_kal_postlex(cst_utterance *u)
{
    cmu_postlex(u);
    fix_ah(u);

    return u;
}

