/*

Read Route Record

Copyright (C) 2019-2020 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef RRR_PERL5_H
#define RRR_PERL5_H

typedef struct av AV;
typedef struct hv HV;
typedef struct sv SV;
typedef struct interpreter PerlInterpreter;
struct rrr_socket_msg;
struct rrr_message;
struct rrr_message_addr;
struct rrr_instance_settings;
struct rrr_setting;

struct rrr_perl5_ctx {
	struct rrr_perl5_ctx *next;
	PerlInterpreter *interpreter;
	void *private_data;

	int (*send_message)(struct rrr_message *message, const struct rrr_message_addr *message_addr, void *private_data);
	char *(*get_setting)(const char *key, void *private_data);
	int (*set_setting)(const char *key, const char *value, void *private_data);
};

// TODO : Consider removing this struct, it only has one field
struct rrr_perl5_message_hv {
	HV *hv;
};

struct rrr_perl5_settings_hv {
	HV *hv;
    SV **entries;
    char **keys;

    int allocated_entries;
    int used_entries;
};

int rrr_perl5_init3(int argc, char **argv, char **env);
int rrr_perl5_sys_term(void);

void rrr_perl5_destroy_ctx (struct rrr_perl5_ctx *ctx);
int rrr_perl5_new_ctx (
		struct rrr_perl5_ctx **target,
		void *private_data,
		int (*send_message) (struct rrr_message *message, const struct rrr_message_addr *message_addr, void *private_data),
		char *(*get_setting) (const char *key, void *private_data),
		int (*set_setting) (const char *key, const char *value, void *private_data)
);
int rrr_perl5_ctx_parse (struct rrr_perl5_ctx *ctx, char *filename, int include_build_dirs);
int rrr_perl5_ctx_run (struct rrr_perl5_ctx *ctx);
int rrr_perl5_call_blessed_hvref (struct rrr_perl5_ctx *ctx, const char *sub, const char *class, HV *hv);

struct rrr_perl5_message_hv *rrr_perl5_allocate_message_hv (struct rrr_perl5_ctx *ctx);

void rrr_perl5_destruct_settings_hv (
		struct rrr_perl5_ctx *ctx,
		struct rrr_perl5_settings_hv *source
);
int rrr_perl5_settings_to_hv (
		struct rrr_perl5_settings_hv **target,
		struct rrr_perl5_ctx *ctx,
		struct rrr_instance_settings *source
);
void rrr_perl5_destruct_message_hv (
		struct rrr_perl5_ctx *ctx,
		struct rrr_perl5_message_hv *source
);
int rrr_perl5_hv_to_message (
		struct rrr_message **target_final,
		struct rrr_message_addr *target_addr,
		struct rrr_perl5_ctx *ctx,
		struct rrr_perl5_message_hv *source
);
int rrr_perl5_message_to_hv (
		struct rrr_perl5_message_hv *message_hv,
		struct rrr_perl5_ctx *ctx,
		const struct rrr_message *message,
		struct rrr_message_addr *message_addr
);
int rrr_perl5_message_to_new_hv (
		struct rrr_perl5_message_hv **target,
		struct rrr_perl5_ctx *ctx,
		const struct rrr_message *message,
		struct rrr_message_addr *message_addr
);

/* Called from XSUB */
int rrr_perl5_message_send (HV *message);
SV *rrr_perl5_settings_get (HV *settings, const char *key);
int rrr_perl5_settings_set (HV *settings, const char *key, const char *value);
int rrr_perl5_debug_msg (HV *debug, int debuglevel, const char *string);
int rrr_perl5_debug_dbg (HV *debug, int debuglevel, const char *string);
int rrr_perl5_debug_err (HV *debug, const char *string);


#endif /* RRR_PERL5_H */