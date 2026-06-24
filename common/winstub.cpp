//
// Copyright 2020 Electronic Arts Inc.
//
// TiberianDawn.DLL and RedAlert.dll and corresponding source code is free
// software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.

// TiberianDawn.DLL and RedAlert.dll and corresponding source code is distributed
// in the hope that it will be useful, but with permitted additional restrictions
// under Section 7 of the GPL. See the GNU General Public License in LICENSE.TXT
// distributed with this program. You should have received a copy of the
// GNU General Public License along with permitted additional restrictions
// with this program. If not, see https://github.com/electronicarts/CnC_Remastered_Collection

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ************************************************************************************************/

#include "winstub.h"

#include "iff.h"
#include "gbuffer.h"
#include "filepcx.h"
#include "debugstring.h"

/***********************************************************************************************
 * Load_Title_Screen -- loads the title screen into the given video buffer                     *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:    screen name                                                                       *
 *           video buffer                                                                      *
 *           ptr to buffer for palette                                                         *
 *                                                                                             *
 * OUTPUT:   Nothing                                                                           *
 *                                                                                             *
 * WARNINGS: None                                                                              *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *    7/5/96 11:30AM ST : Created                                                              *
 *=============================================================================================*/

void Load_Title_Screen(const char* name, GraphicViewPortClass* video_page, unsigned char* palette)
{
    printf("[TS] Load_Title_Screen('%s') vp=%dx%d\n",
           name ? name : "(null)", video_page->Get_Width(), video_page->Get_Height());
    if (!name) return;
    GraphicBufferClass* load_buffer = nullptr;
    const char* ext = strrchr(name, '.');

    if (!strcasecmp(ext, ".PCX")) {
        load_buffer = Read_PCX_File(name, (char*)palette, NULL, 0);
        if (!load_buffer) {
            /* PCX not found — fall back to TITLE.CPS (DOS 320x200 version in CONQUER.MIX). */
            printf("[TS] PCX not found, falling back to TITLE.CPS\n");
            const int width = 320, height = 200;
            load_buffer = new GraphicBufferClass(width, height, NULL, width * (height + 4));
            unsigned char temp_pal[768] = {0};
            Load_Uncompress("TITLE.CPS", *load_buffer, *load_buffer, temp_pal);
            bool has_pal = false;
            for (int i = 0; i < 768; i++) { if (temp_pal[i]) { has_pal = true; break; } }
            if (has_pal) memcpy(palette, temp_pal, 768);
            if (!load_buffer->Get_Buffer()) { delete load_buffer; load_buffer = nullptr; }
        }
    } else if (!strcasecmp(ext, ".CPS")) {
        /* CPS files are hardcoded to 320x200. */
        const int width = 320;
        const int height = 200;

        load_buffer = new GraphicBufferClass(width, height, NULL, width * (height + 4));
        /* Load into a temp palette so an all-zero embedded palette doesn't clobber GamePalette. */
        unsigned char temp_pal[768] = {0};
        Load_Uncompress(name, *load_buffer, *load_buffer, temp_pal);
        bool has_pal = false;
        for (int i = 0; i < 768; i++) { if (temp_pal[i]) { has_pal = true; break; } }
        printf("[title] %s loaded, has_pal=%d pal[3]=%d\n", name, has_pal, temp_pal[3]);
        if (has_pal) memcpy(palette, temp_pal, 768);
    } else {
        /* Invalid title screen. */
        DBG_ERROR("Title screen file %s do not have PCX or CPS extension", name);
    }

    if (!load_buffer) {
        printf("[TS] load_buffer is NULL — '%s' failed to load\n", name);
        return;
    }
    {
        int sw = load_buffer->Get_Width();
        int sh = load_buffer->Get_Height();
        int dw = video_page->Get_Width();
        int dh = video_page->Get_Height();
        printf("[TS] src=%dx%d dst=%dx%d\n", sw, sh, dw, dh);

        if (sw == dw && sh == dh) {
            load_buffer->Blit(*video_page);
        } else if (video_page->Lock()) {
            /* Nearest-neighbour stretch to fill the full viewport (e.g. 320×200 → 640×400). */
            int d_stride = dw + video_page->Get_XAdd() + video_page->Get_Pitch();
            uint8_t* dst  = reinterpret_cast<uint8_t*>(video_page->Get_Offset());
            const uint8_t* src = reinterpret_cast<const uint8_t*>(load_buffer->Get_Buffer());
            for (int dy = 0; dy < dh; dy++) {
                int sy = dy * sh / dh;
                const uint8_t* src_row = src + sy * sw;
                uint8_t* dst_row = dst + dy * d_stride;
                for (int dx = 0; dx < dw; dx++)
                    dst_row[dx] = src_row[dx * sw / dw];
            }
            video_page->Unlock();
        }
        delete load_buffer;
    }
}
