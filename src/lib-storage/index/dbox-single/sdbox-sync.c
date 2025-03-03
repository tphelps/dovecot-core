/* Copyright (c) 2007-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "dbox-attachment.h"
#include "sdbox-storage.h"
#include "sdbox-file.h"
#include "sdbox-sync.h"
#include "mailbox-recent-flags.h"

#define SDBOX_REBUILD_COUNT 3

static void
dbox_sync_file_move_if_needed(struct dbox_file *file, struct event *event,
			      enum sdbox_sync_entry_type type)
{
	struct stat st;
	bool move_to_alt = type == SDBOX_SYNC_ENTRY_TYPE_MOVE_TO_ALT;
	bool deleted;

	if (move_to_alt == dbox_file_is_in_alt(file) &&
	    !move_to_alt) {
		/* unopened dbox files default to primary dir.
		   stat the file to update its location. */
		(void)dbox_file_stat(file, event, &st);

	}
	if (move_to_alt != dbox_file_is_in_alt(file)) {
		/* move the file. if it fails, nothing broke so
		   don't worry about it. */
		if (dbox_file_open(file, &deleted) > 0 && !deleted)
			(void)sdbox_file_move(file, move_to_alt);
	}
}

static void sdbox_sync_file(struct sdbox_sync_context *ctx,
			    uint32_t seq, uint32_t uid,
			    enum sdbox_sync_entry_type type)
{
	struct dbox_file *file;
	enum modify_type modify_type;

	switch (type) {
	case SDBOX_SYNC_ENTRY_TYPE_EXPUNGE:
		if (!mail_index_transaction_is_expunged(ctx->trans, seq)) {
			mail_index_expunge(ctx->trans, seq);
			array_push_back(&ctx->expunged_uids, &uid);
		}
		break;
	case SDBOX_SYNC_ENTRY_TYPE_MOVE_FROM_ALT:
	case SDBOX_SYNC_ENTRY_TYPE_MOVE_TO_ALT:
		/* update flags in the sync transaction, mainly to make
		   sure that these alt changes get marked as synced
		   and won't be retried */
		modify_type = type == SDBOX_SYNC_ENTRY_TYPE_MOVE_TO_ALT ?
			MODIFY_ADD : MODIFY_REMOVE;
		mail_index_update_flags(ctx->trans, seq, modify_type,
					(enum mail_flags)DBOX_INDEX_FLAG_ALT);
		file = sdbox_file_init(ctx->mbox, uid);
		dbox_sync_file_move_if_needed(file, ctx->mbox->box.event, type);
		dbox_file_unref(&file);
		break;
	}
}

static void sdbox_sync_add(struct sdbox_sync_context *ctx,
			   const struct mail_index_sync_rec *sync_rec)
{
	uint32_t uid;
	enum sdbox_sync_entry_type type;
	uint32_t seq, seq1, seq2;

	if (sync_rec->type == MAIL_INDEX_SYNC_TYPE_EXPUNGE) {
		/* we're interested */
		type = SDBOX_SYNC_ENTRY_TYPE_EXPUNGE;
	} else if (sync_rec->type == MAIL_INDEX_SYNC_TYPE_FLAGS) {
		/* we care only about alt flag changes */
		if ((sync_rec->add_flags & DBOX_INDEX_FLAG_ALT) != 0)
			type = SDBOX_SYNC_ENTRY_TYPE_MOVE_TO_ALT;
		else if ((sync_rec->remove_flags & DBOX_INDEX_FLAG_ALT) != 0)
			type = SDBOX_SYNC_ENTRY_TYPE_MOVE_FROM_ALT;
		else
			return;
	} else {
		/* not interested */
		return;
	}

	if (!mail_index_lookup_seq_range(ctx->sync_view,
					 sync_rec->uid1, sync_rec->uid2,
					 &seq1, &seq2)) {
		/* already expunged everything. nothing to do. */
		return;
	}

	for (seq = seq1; seq <= seq2; seq++) T_BEGIN {
		mail_index_lookup_uid(ctx->sync_view, seq, &uid);
		sdbox_sync_file(ctx, seq, uid, type);
	} T_END;
}

static int sdbox_sync_index(struct sdbox_sync_context *ctx)
{
	struct mailbox *box = &ctx->mbox->box;
	const struct mail_index_header *hdr;
	struct mail_index_sync_rec sync_rec;
	uint32_t seq1, seq2;

	hdr = mail_index_get_header(ctx->sync_view);
	if (hdr->uid_validity == 0) {
		/* newly created index file */
		if (hdr->next_uid == 1) {
			/* could be just a race condition where we opened the
			   mailbox between mkdir and index creation. fix this
			   silently. */
			if (sdbox_mailbox_create_indexes(box, NULL, ctx->trans) < 0)
				return -1;
			return 1;
		}
		mailbox_set_critical(box,
			"sdbox: Broken index: missing UIDVALIDITY");
		sdbox_set_mailbox_corrupted(box);
		return 0;
	}

	/* mark the newly seen messages as recent */
	if (mail_index_lookup_seq_range(ctx->sync_view, hdr->first_recent_uid,
					hdr->next_uid, &seq1, &seq2))
		mailbox_recent_flags_set_seqs(box, ctx->sync_view, seq1, seq2);

	while (mail_index_sync_next(ctx->index_sync_ctx, &sync_rec))
		sdbox_sync_add(ctx, &sync_rec);
	return 1;
}

static void dbox_sync_file_expunge(struct sdbox_sync_context *ctx,
				   uint32_t uid)
{
	struct mailbox *box = &ctx->mbox->box;
	struct dbox_file *file;
	struct sdbox_file *sfile;
	int ret;

	file = sdbox_file_init(ctx->mbox, uid);
	sfile = (struct sdbox_file *)file;
	if (file->storage->attachment_dir != NULL)
		ret = sdbox_file_unlink_with_attachments(sfile);
	else
		ret = dbox_file_unlink(file);

	/* do sync_notify only when the file was unlinked by us */
	if (ret > 0)
		mailbox_sync_notify(box, uid, MAILBOX_SYNC_TYPE_EXPUNGE);
	dbox_file_unref(&file);
}

static void dbox_sync_expunge_files(struct sdbox_sync_context *ctx)
{
	uint32_t uid;

	/* NOTE: Index is no longer locked. Multiple processes may be unlinking
	   the files at the same time. */
	ctx->mbox->box.tmp_sync_view = ctx->sync_view;
	array_foreach_elem(&ctx->expunged_uids, uid) T_BEGIN {
		dbox_sync_file_expunge(ctx, uid);
	} T_END;
	mailbox_sync_notify(&ctx->mbox->box, 0, 0);
	ctx->mbox->box.tmp_sync_view = NULL;
}

static int
sdbox_refresh_header(struct sdbox_mailbox *mbox, bool retry, bool log_error)
{
	struct mail_index_view *view;
	struct sdbox_index_header hdr;
	bool need_resize;
	int ret;

	view = mail_index_view_open(mbox->box.index);
	ret = sdbox_read_header(mbox, &hdr, log_error, &need_resize);
	mail_index_view_close(&view);

	if (ret < 0 && retry) {
		mail_index_refresh(mbox->box.index);
		return sdbox_refresh_header(mbox, FALSE, log_error);
	}
	return ret;
}

int sdbox_sync_begin(struct sdbox_mailbox *mbox, enum sdbox_sync_flags flags,
		     struct sdbox_sync_context **ctx_r)
{
	const struct mail_index_header *hdr =
		mail_index_get_header(mbox->box.view);
	struct sdbox_sync_context *ctx;
	enum mail_index_sync_flags sync_flags;
	unsigned int i;
	int ret;
	bool rebuild, force_rebuild;

	force_rebuild = (flags & SDBOX_SYNC_FLAG_FORCE_REBUILD) != 0;
	rebuild = force_rebuild ||
		(hdr->flags & MAIL_INDEX_HDR_FLAG_FSCKD) != 0 ||
		mbox->corrupted_rebuild_count != 0 ||
		sdbox_refresh_header(mbox, TRUE, FALSE) < 0;

	ctx = i_new(struct sdbox_sync_context, 1);
	ctx->mbox = mbox;
	ctx->flags = flags;
	i_array_init(&ctx->expunged_uids, 32);

	sync_flags = index_storage_get_sync_flags(&mbox->box);
	if (!rebuild && (flags & SDBOX_SYNC_FLAG_FORCE) == 0)
		sync_flags |= MAIL_INDEX_SYNC_FLAG_REQUIRE_CHANGES;
	if ((flags & SDBOX_SYNC_FLAG_FSYNC) != 0)
		sync_flags |= MAIL_INDEX_SYNC_FLAG_FSYNC;
	/* don't write unnecessary dirty flag updates */
	sync_flags |= MAIL_INDEX_SYNC_FLAG_AVOID_FLAG_UPDATES;

	for (i = 0;; i++) {
		ret = index_storage_expunged_sync_begin(&mbox->box,
				&ctx->index_sync_ctx, &ctx->sync_view,
				&ctx->trans, sync_flags);
		if (mail_index_reset_fscked(mbox->box.index))
			sdbox_set_mailbox_corrupted(&mbox->box);
		if (ret <= 0) {
			array_free(&ctx->expunged_uids);
			i_free(ctx);
			*ctx_r = NULL;
			return ret;
		}

		if (rebuild)
			ret = 0;
		else {
			if ((ret = sdbox_sync_index(ctx)) > 0)
				break;
		}

		/* failure. keep the index locked while we're doing a
		   rebuild. */
		if (ret == 0) {
			if (i >= SDBOX_REBUILD_COUNT) {
				mailbox_set_critical(&ctx->mbox->box,
					"sdbox: Index keeps breaking");
				ret = -1;
			} else {
				/* do a full resync and try again. */
				rebuild = FALSE;
				ret = sdbox_sync_index_rebuild(mbox,
							       force_rebuild);
			}
		}
		mail_index_sync_rollback(&ctx->index_sync_ctx);
		if (ret < 0) {
			index_storage_expunging_deinit(&ctx->mbox->box);
			array_free(&ctx->expunged_uids);
			i_free(ctx);
			return -1;
		}
	}

	*ctx_r = ctx;
	return 0;
}

int sdbox_sync_finish(struct sdbox_sync_context **_ctx, bool success)
{
	struct sdbox_sync_context *ctx = *_ctx;
	struct mail_storage *storage = &ctx->mbox->storage->storage.storage;
	int ret = success ? 0 : -1;

	*_ctx = NULL;

	if (success) {
		mail_index_view_ref(ctx->sync_view);

		if (mail_index_sync_commit(&ctx->index_sync_ctx) < 0) {
			mailbox_set_index_error(&ctx->mbox->box);
			ret = -1;
		} else {
			dbox_sync_expunge_files(ctx);
		}
		mail_index_view_close(&ctx->sync_view);
	} else {
		mail_index_sync_rollback(&ctx->index_sync_ctx);
	}

        if (storage->rebuild_list_index)
		ret = mail_storage_list_index_rebuild_and_set_uncorrupted(storage);

	index_storage_expunging_deinit(&ctx->mbox->box);
	array_free(&ctx->expunged_uids);
	i_free(ctx);
	return ret;
}

int sdbox_sync(struct sdbox_mailbox *mbox, enum sdbox_sync_flags flags)
{
	struct sdbox_sync_context *sync_ctx;

	if (sdbox_sync_begin(mbox, flags, &sync_ctx) < 0)
		return -1;

	if (sync_ctx == NULL)
		return 0;
	return sdbox_sync_finish(&sync_ctx, TRUE);
}

struct mailbox_sync_context *
sdbox_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct sdbox_mailbox *mbox = SDBOX_MAILBOX(box);
	enum sdbox_sync_flags sdbox_sync_flags = 0;
	int ret = 0;

	if (mail_index_reset_fscked(box->index))
		sdbox_set_mailbox_corrupted(box);
	if (index_mailbox_want_full_sync(&mbox->box, flags) ||
	    mbox->corrupted_rebuild_count != 0) {
		if ((flags & MAILBOX_SYNC_FLAG_FORCE_RESYNC) != 0)
			sdbox_sync_flags |= SDBOX_SYNC_FLAG_FORCE_REBUILD;
		ret = sdbox_sync(mbox, sdbox_sync_flags);
	}

	return index_mailbox_sync_init(box, flags, ret < 0);
}
