#include "font.h"
#include "ui/config.h"
#include "i18n.h"
#include "res.h"

#include <fontconfig/fontconfig.h>


static bool fontset_load_fc(app_fontset_t *set, FcPattern *font);

static bool fontset_load_mem(app_fontset_t *set, const char *name, const void *mem, size_t size);

static void fontset_destroy_fonts(app_fontset_t *fontset);

int app_font_init(app_fonts_t *fonts, int dpi) {
    app_fontset_t fontset = {
            .small_size = _LV_DPX_CALC(dpi, 14),
            .normal_size = _LV_DPX_CALC(dpi, 16),
            .large_size = _LV_DPX_CALC(dpi, 19),
            .display_size = _LV_DPX_CALC(dpi, 48)
    };
    app_fontset_t iconfonts = fontset;
    if (!fontset_load_mem(&iconfonts, "MaterialIcons", res_mat_iconfont_data, res_mat_iconfont_size)) {
        return -1;
    }
    //does not necessarily have to be a specific name.  You could put anything here and Fontconfig WILL find a font for you
    FcPattern *pattern = FcNameParse((const FcChar8 *) FONT_FAMILY);
    if (!pattern) {
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }

    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;

    FcPattern *font = FcFontMatch(NULL, pattern, &result);
    if (font == NULL) {
        FcPatternDestroy(pattern);
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
    if (!fontset_load_fc(&fontset, font)) {
        FcPatternDestroy(font);
        FcPatternDestroy(pattern);
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
    if (font) {
        FcPatternDestroy(font);
        font = NULL;
    }
    FcPatternDestroy(pattern);
    pattern = NULL;
#ifdef FONT_FAMILY_FALLBACK
    const i18n_entry_t *loc_entry = i18n_entry(i18n_locale());
    pattern = FcNameParse((const FcChar8 *) ((loc_entry && loc_entry->font) ? loc_entry->font : FONT_FAMILY_FALLBACK));
    if (pattern != NULL) {
        FcLangSet *ls = FcLangSetCreate();
        if (loc_entry) {
            FcLangSetAdd(ls, (const FcChar8 *) loc_entry->locale);
            FcPatternAddLangSet(pattern, FC_LANG, ls);
        }

        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        font = FcFontMatch(NULL, pattern, &result);
        if (font != NULL) {
            fontset.fallback = calloc(1, sizeof(app_fontset_t));
            fontset.fallback->small_size = fontset.small_size;
            fontset.fallback->normal_size = fontset.normal_size;
            fontset.fallback->large_size = fontset.large_size;
            fontset.fallback->display_size = fontset.display_size;
            if (fontset_load_fc(fontset.fallback, font)) {
                fontset.normal->fallback = fontset.fallback->normal;
                fontset.large->fallback = fontset.fallback->large;
                fontset.small->fallback = fontset.fallback->small;
                fontset.display->fallback = fontset.fallback->display;
            } else {
                free(fontset.fallback);
                fontset.fallback = NULL;
            }
            FcPatternDestroy(font);
            font = NULL;
        }
        FcLangSetDestroy(ls);
        FcPatternDestroy(pattern);
        pattern = NULL;
    }
#endif
    fonts->fonts = fontset;
    fonts->icons = iconfonts;
    return 0;
}

void app_font_deinit(app_fonts_t *fonts) {
    fontset_destroy_fonts(&fonts->fonts);
    fontset_destroy_fonts(&fonts->icons);
}

static lv_font_t *font_load_file(const FcChar8 *path, int size) {
    lv_ft_info_t info = {.name=(const char *) path, .style=FT_FONT_STYLE_NORMAL, .weight=size};
    return lv_ft_font_init(&info) ? info.font : NULL;
}

static bool fontset_load_fc(app_fontset_t *set, FcPattern *font) {
    FcChar8 *file = NULL, *family = NULL, *bold_file = NULL;
    if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch) return false;

    /* Match a real bold face; a style flag on the regular file depends on the
     * rasterizer/cache and did not reproduce the selected title's weight. */
    FcPattern *query = FcPatternCreate(), *bold = NULL;
    if (query && FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch) {
        FcPatternAddString(query, FC_FAMILY, family);
        FcPatternAddInteger(query, FC_WEIGHT, FC_WEIGHT_BOLD);
        FcConfigSubstitute(NULL, query, FcMatchPattern);
        FcDefaultSubstitute(query);
        FcResult result;
        bold = FcFontMatch(NULL, query, &result);
    }
    if (!bold || FcPatternGetString(bold, FC_FILE, 0, &bold_file) != FcResultMatch) bold_file = file;
    set->normal = font_load_file(file, set->normal_size);
    set->small = font_load_file(file, set->small_size);
    set->large = font_load_file(bold_file, set->large_size);
    set->display = font_load_file(bold_file, set->display_size);
    if (bold) FcPatternDestroy(bold);
    if (query) FcPatternDestroy(query);
    if (set->normal && set->small && set->large && set->display) return true;
    fontset_destroy_fonts(set);
    return false;
}

static bool fontset_load_mem(app_fontset_t *set, const char *name, const void *mem, size_t size) {
    lv_ft_info_t ft_info = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->normal_size};
    if (lv_ft_font_init(&ft_info)) {
        set->normal = ft_info.font;
    }
    lv_ft_info_t ft_info_lg = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->large_size};
    if (lv_ft_font_init(&ft_info_lg)) {
        set->large = ft_info_lg.font;
    }
    lv_ft_info_t ft_info_sm = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->small_size};
    if (lv_ft_font_init(&ft_info_sm)) {
        set->small = ft_info_sm.font;
    }
    lv_ft_info_t display = {.name=name, .mem=mem, .mem_size=size, .style=FT_FONT_STYLE_NORMAL,
                           .weight=set->display_size};
    if (lv_ft_font_init(&display)) set->display = display.font;
    if (!set->small || !set->normal || !set->large || !set->display) {
        if (set->small) {
            lv_ft_font_destroy(set->small);
            set->small = NULL;
        }
        if (set->normal) {
            lv_ft_font_destroy(set->normal);
            set->normal = NULL;
        }
        if (set->large) {
            lv_ft_font_destroy(set->large);
            set->large = NULL;
        }
        if (set->display) {
            lv_ft_font_destroy(set->display);
            set->display = NULL;
        }
        return false;
    }
    return true;
}

static void fontset_destroy_fonts(app_fontset_t *fontset) {
    lv_ft_font_destroy(fontset->large);
    lv_ft_font_destroy(fontset->normal);
    lv_ft_font_destroy(fontset->small);
    lv_ft_font_destroy(fontset->display);
    app_fontset_t *fallback = fontset->fallback;
    if (fallback) {
        lv_ft_font_destroy(fallback->large);
        lv_ft_font_destroy(fallback->normal);
        lv_ft_font_destroy(fallback->small);
        lv_ft_font_destroy(fallback->display);
        free(fallback);
    }
}
