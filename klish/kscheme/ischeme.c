#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <faux/str.h>
#include <faux/list.h>
#include <faux/error.h>
#include <klish/kview.h>
#include <klish/kscheme.h>


bool_t kview_nested_from_iview(kview_t *kview, iview_t *iview,
	faux_error_t *error_stack)
{
	if (!kview || !iview) {
		if (error_stack)
			faux_error_add(error_stack,
				kview_strerror(KVIEW_ERROR_INTERNAL));
		return BOOL_FALSE;
	}

	// Command list
	if (iview->commands) {
		icommand_t **p_icommand = NULL;
		for (p_icommand = *iview->commands; *p_icommand; p_icommand++) {
			kcommand_t *kcommand = NULL;
			icommand_t *icommand = *p_icommand;
printf("command %s\n", icommand->name);
//			kcommand = kcommand_from_icommand(icommand, error_stack);
//			if (!kcommand)
//				continue;
kcommand = kcommand;
		}
	}

	return BOOL_TRUE;
}


kview_t *kview_from_iview(iview_t *iview, faux_error_t *error_stack)
{
	kview_t *kview = NULL;
	kview_error_e kview_error = KVIEW_ERROR_OK;
	ssize_t error_stack_len = 0;

	kview = kview_new(iview, &kview_error);
	if (!kview) {
		if (error_stack) {
			char *msg = NULL;
			msg = faux_str_sprintf("VIEW \"%s\": %s",
				iview->name ? iview->name : "(null)",
				kview_strerror(kview_error));
			faux_error_add(error_stack, msg);
			faux_str_free(msg);
		}
		return NULL;
	}
	printf("view %s\n", kview_name(kview));

	// Parse nested elements
	if (error_stack)
		error_stack_len = faux_error_len(error_stack);
	kview_nested_from_iview(kview, iview, error_stack);
	if (error_stack && (faux_error_len(error_stack) > error_stack_len)) {
		char *msg = NULL;
		msg = faux_str_sprintf("VIEW \"%s\": Illegal nested elements",
			kview_name(kview));
		faux_error_add(error_stack, msg);
		faux_str_free(msg);
	}

	return kview;
}


bool_t kscheme_nested_from_ischeme(kscheme_t *kscheme, ischeme_t *ischeme,
	faux_error_t *error_stack)
{
	if (!kscheme || !ischeme) {
		if (error_stack)
			faux_error_add(error_stack,
				kscheme_strerror(KSCHEME_ERROR_INTERNAL));
		return BOOL_FALSE;
	}

	// View list
	if (ischeme->views) {
		iview_t **p_iview = NULL;
		for (p_iview = *ischeme->views; *p_iview; p_iview++) {
			kview_t *kview = NULL;
			iview_t *iview = *p_iview;

			kview = kview_from_iview(iview, error_stack);
			if (!kview)
				continue;
			kscheme_add_view(kscheme, kview);
		}
	}

	return BOOL_TRUE;
}


kscheme_t *kscheme_from_ischeme(ischeme_t *ischeme, faux_error_t *error_stack)
{
	kscheme_t *kscheme = NULL;
	kscheme_error_e kscheme_error = KSCHEME_ERROR_OK;

	kscheme = kscheme_new(&kscheme_error);
	if (!kscheme) {
		if (error_stack)
			faux_error_add(error_stack,
				kscheme_strerror(kscheme_error));
		return NULL;
	}

	kscheme_nested_from_ischeme(kscheme, ischeme, error_stack);

	return kscheme;
}
