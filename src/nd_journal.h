/*
 * nd_journal.h - journal and checkpoint interfaces.
 */

#ifndef ND_JOURNAL_H
#define ND_JOURNAL_H

#include "nd_common.h"

/*
 * Journal mode identifier.
 */
enum nd_journal_type
{
  ND_JOURNAL_NONE = 0,
  ND_JOURNAL_SESSION_SCOPED = 1,
  ND_JOURNAL_GOBACK = 2
};

struct nd_journal;

/*
 * Journal lifecycle helpers.
 */
int nd_journal_open_goback (const char *image_path, struct nd_journal **out_journal,
                            char *err, size_t errlen);
int nd_journal_open_session_scoped (const char *image_path,
                                    struct nd_journal **out_journal,
                                    char *err, size_t errlen);
int nd_journal_close (struct nd_journal *journal, char *err, size_t errlen);
void nd_journal_destroy (struct nd_journal *journal);

/*
 * Sector redirection and checkpoint entry points.
 */
enum nd_journal_type nd_journal_type (const struct nd_journal *journal);
int nd_journal_has_lba (const struct nd_journal *journal, uint32_t lba);
int nd_journal_read_lba (const struct nd_journal *journal, uint32_t lba,
                         uint8_t *buffer, char *err, size_t errlen);
int nd_journal_write_lba (struct nd_journal *journal, uint32_t lba,
                          const uint8_t *buffer, char *err, size_t errlen);

int nd_journal_mark_checkpoint (struct nd_journal *journal,
                                const uint8_t *tag, size_t tag_len,
                                char *err, size_t errlen);
int nd_journal_goto_checkpoint (struct nd_journal *journal,
                                const uint8_t *payload, size_t payload_len,
                                char *message, size_t message_len);
int nd_journal_list_checkpoints (struct nd_journal *journal,
                                 uint8_t *payload, size_t *payload_len,
                                 char *err, size_t errlen);

#endif
