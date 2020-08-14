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

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util/linked_list.h"
#include "array_tree.h"
#include "array.h"
#include "type.h"
#include "parse.h"
#include "util/rrr_time.h"

static void __rrr_array_branch_destroy(
		struct rrr_array_branch *branch
) {
	rrr_condition_clear(&branch->condition);
	if (branch->array_tree != NULL) {
		rrr_array_tree_destroy(branch->array_tree);
	}
	RRR_LL_DESTROY(&branch->branches_elsif, struct rrr_array_branch, __rrr_array_branch_destroy(node));
	if (branch->tree_else != NULL) {
		rrr_array_tree_destroy(branch->tree_else);
	}
	free(branch);
}

static void __rrr_array_node_destroy (
		struct rrr_array_node *node
) {
	rrr_array_clear(&node->array);
	if (node->branch_if != NULL) {
		__rrr_array_branch_destroy(node->branch_if);
	}
	free(node);
}

void rrr_array_tree_clear (
		struct rrr_array_tree *tree
) {
	RRR_LL_DESTROY(tree, struct rrr_array_node, __rrr_array_node_destroy(node));
	RRR_FREE_IF_NOT_NULL(tree->name);
}

void rrr_array_tree_destroy (
		struct rrr_array_tree *tree
) {
	 rrr_array_tree_clear(tree);
	free(tree);
}

int rrr_array_tree_new (
		struct rrr_array_tree **target,
		const char *name
) {
	int ret = 0;

	struct rrr_array_tree *new_tree = malloc(sizeof(*new_tree));
	if (new_tree == NULL) {
		RRR_MSG_0("Could not allocate memory in rrr_array_tree_clone\n");
		ret = 1;
		goto out;
	}

	memset(new_tree, '\0', sizeof(*new_tree));

	if ((new_tree->name = strdup(name != NULL ? name : "-")) == NULL) {
		RRR_MSG_0("Could not allocate memory in rrr_array_tree_clone\n");
		ret = 1;
		goto out;
	}

	*target = new_tree;
	new_tree = NULL;

	out:
	if (new_tree != NULL) {
		free(new_tree);
	}
	return ret;
}

void rrr_array_tree_list_clear (
		struct rrr_array_tree_list *list
) {
	RRR_LL_DESTROY(list, struct rrr_array_tree, rrr_array_tree_destroy(node));
}

const struct rrr_array_tree *rrr_array_tree_list_get_tree_by_name (
		const struct rrr_array_tree_list *list,
		const char *name
) {
	RRR_LL_ITERATE_BEGIN(list, const struct rrr_array_tree);
		if (node->name != NULL && strcmp(node->name, name) == 0) {
			return node;
		}
	RRR_LL_ITERATE_END();
	return NULL;
}

static struct rrr_array_branch *__rrr_array_branch_allocate(void) {
	struct rrr_array_branch *branch = malloc(sizeof(*branch));
	if (branch == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_array_branch_allocate\n");
		return NULL;
	}
	memset(branch, '\0', sizeof(*branch));
	return branch;
}

static struct rrr_array_node *__rrr_array_node_allocate (void) {
	struct rrr_array_node *node = malloc(sizeof(*node));
	if (node == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_array_node_allocate\n");
		return NULL;
	}
	memset(node, '\0', sizeof(*node));
	return node;
}

static int __rrr_array_branch_clone (
		struct rrr_array_branch **target,
		const struct rrr_array_branch *source
) {
	int ret = 0;

	struct rrr_array_branch *new_branch = __rrr_array_branch_allocate();
	if (new_branch == NULL) {
		ret = 1;
		goto out;
	}

	if ((ret = rrr_condition_clone(&new_branch->condition, &source->condition)) != 0) {
		goto out;
	}

	if (source->array_tree != NULL) {
		if ((ret = rrr_array_tree_clone(&new_branch->array_tree, source->array_tree)) != 0) {
			goto out;
		}
	}

	RRR_LL_ITERATE_BEGIN(&source->branches_elsif, const struct rrr_array_branch);
		struct rrr_array_branch *new_branch_elsif = NULL;
		if ((ret = __rrr_array_branch_clone(&new_branch_elsif, node)) != 0) {
			goto out;
		}
	RRR_LL_ITERATE_END();

	if (source->tree_else != NULL) {
		if ((ret = rrr_array_tree_clone(&new_branch->tree_else, source->tree_else)) != 0) {
			goto out;
		}
	}

	*target = new_branch;
	new_branch = NULL;

	out:
	if (new_branch != NULL) {
		__rrr_array_branch_destroy(new_branch);
	}
	return ret;
}

static int __rrr_array_node_clone (
		struct rrr_array_node **target,
		const struct rrr_array_node *source
) {
	int ret = 0;

	*target = NULL;

	struct rrr_array_node *new_node =__rrr_array_node_allocate();
	if (new_node == NULL) {
		ret = 1;
		goto out;
	}

	if ((ret = rrr_array_definition_clone(&new_node->array, &source->array)) != 0) {
		goto out;
	}

	if (source->branch_if != NULL) {
		if ((ret = __rrr_array_branch_clone(&new_node->branch_if, source->branch_if)) != 0) {
			goto out;
		}
	}

	*target = new_node;
	new_node = NULL;

	out:
	if (new_node != NULL) {
		__rrr_array_node_destroy(new_node);
	}
	return ret;
}

int rrr_array_tree_push_array_clear_source (
		struct rrr_array_tree *target,
		struct rrr_array *source
) {
	struct rrr_array_node *node = __rrr_array_node_allocate();
	if (node == NULL) {
		return 1;
	}

	RRR_LL_MERGE_AND_CLEAR_SOURCE_HEAD(&node->array, source);

	RRR_LL_PUSH(target, node);

	return 0;
}

static int __rrr_array_tree_parse_if (
		struct rrr_array_branch **target,
		struct rrr_parse_pos *pos
) {
	int ret = 0;

	*target = NULL;

	struct rrr_array_branch *branch;
	if ((branch = __rrr_array_branch_allocate()) == NULL) {
		ret = RRR_ARRAY_TREE_HARD_ERROR;
		goto out;
	}

	if ((ret = rrr_condition_parse(&branch->condition, pos)) != 0) {
		goto out_destroy_branch;
	}

	if ((ret = rrr_array_tree_parse(&branch->array_tree, pos, NULL)) != 0) {
		goto out_destroy_branch;
	}

	*target = branch;

	goto out;
	out_destroy_branch:
		__rrr_array_branch_destroy(branch);
	out:
	return ret;
}

static int __rrr_array_tree_parse_if_node (
		struct rrr_array_node **target,
		struct rrr_parse_pos *pos
) {
	int ret = 0;

	*target = NULL;

	struct rrr_array_node *node;
	if ((node = __rrr_array_node_allocate()) == NULL) {
		ret = RRR_ARRAY_TREE_HARD_ERROR;
		goto out;
	}

	if ((ret = __rrr_array_tree_parse_if(&node->branch_if, pos)) != 0) {
		goto out_destroy_node;
	}

	*target = node;

	goto out;
	out_destroy_node:
		__rrr_array_node_destroy(node);
	out:
		return ret;
}

static int __rrr_array_tree_parse_single_definition (
		struct rrr_array *target,
		const char *start,
		const char *end
) {
	int ret = 0;

	rrr_length parsed_bytes = 0;
	const struct rrr_type_definition *type = NULL;
	unsigned int length = 0;
	char *length_ref = NULL;
	unsigned int item_count = 0;
	char *item_count_ref = NULL;
	rrr_type_flags flags = 0;
	const char *tag_start = NULL;
	unsigned int tag_length = 0;

	if ((ret = rrr_array_parse_identifier_and_size (
			&type,
			&length,
			&length_ref,
			&item_count,
			&item_count_ref,
			&flags,
			&parsed_bytes,
			start,
			end
	)) != 0) {
		RRR_MSG_0("Error while parsing type identifier and size\n");
		goto out;
	}

	start += parsed_bytes;

	if (*start == '#') {
		start++;
		tag_start = start;

		while (*start != '\0') {
			if (!RRR_PARSE_MATCH_C_LETTER(*start)) {
				RRR_MSG_0("Invalid character '%c' in tag name (decimal %u)\n", (*start), (unsigned char) (*start));
				ret = RRR_ARRAY_TREE_SOFT_ERROR;
				goto out;
			}
			tag_length++;
			start++;
		}

		if (tag_length == 0) {
			RRR_MSG_0("Missing tag name after #\n");
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out;
		}
	}

	if (*start != '\0') {
		RRR_MSG_0("Extra data after type definition here --> '%s'\n", start);
		ret = RRR_ARRAY_TREE_SOFT_ERROR;
		goto out;
	}

	if (length > type->max_length) {
		RRR_MSG_0("Size argument in type definition '%s' is too large, max is '%u'\n",
				type->identifier, type->max_length);
		ret = RRR_ARRAY_TREE_SOFT_ERROR;
		goto out;
	}

	struct rrr_type_value *template = NULL;

	if (rrr_type_value_new (
			&template,
			type,
			flags,
			tag_length,
			tag_start,
			length,
			length_ref,
			item_count,
			item_count_ref,
			0
	) != 0) {
		RRR_MSG_0("Could not create value in rrr_array_parse_definition\n");
		ret = RRR_ARRAY_TREE_HARD_ERROR;
		goto out;
	}

	RRR_LL_APPEND(target,template);

	out:
	RRR_FREE_IF_NOT_NULL(length_ref);
	RRR_FREE_IF_NOT_NULL(item_count_ref);
	return ret;
}

#define CHECK_BRANCH								\
	do {int pos_orig = pos->pos;					\
	if (rrr_parse_match_word(pos, "IF") ||			\
		rrr_parse_match_word(pos, "ELSIF") ||		\
		rrr_parse_match_word(pos, "ELSE")			\
	) {												\
		pos->pos = pos_orig;						\
		*branch_found = 1;							\
		goto out;									\
	}} while(0)

static void __rrr_array_tree_parse_definition_node_check_end (
		struct rrr_parse_pos *pos,
		int *eof_found,
		int *semicolon_found,
		int *comma_found,
		int *branch_found
) {
	*eof_found = 0;
	*semicolon_found = 0;
	*comma_found = 0;
	*branch_found = 0;

	rrr_parse_ignore_spaces_and_increment_line(pos);
	if (RRR_PARSE_CHECK_EOF(pos)) {
		*eof_found = 1;
		goto out;
	}

	CHECK_BRANCH;

	if (*(pos->data + pos->pos) == ',') {
		*comma_found = 1;

		pos->pos++;

		rrr_parse_ignore_spaces_and_increment_line(pos);
		if (RRR_PARSE_CHECK_EOF(pos)) {
			*eof_found = 1;
			goto out;
		}

		CHECK_BRANCH;

		goto out;
	}
	else if (*(pos->data + pos->pos) == ';') {
		// End of node tree
		*semicolon_found = 1;
		pos->pos++;
		goto out;
	}

	out:
	rrr_parse_ignore_spaces_and_increment_line(pos);
}

static int __rrr_array_tree_parse_definition_node (
		int *semicolon_found,
		struct rrr_array_node **target,
		struct rrr_parse_pos *pos
) {
	int ret = 0;

	*target = NULL;

	int eof_found = 0;
	int comma_found = 0;
	int branch_found = 0;

	struct rrr_array_node *node;
	if ((node = __rrr_array_node_allocate()) == NULL) {
		ret = RRR_ARRAY_TREE_HARD_ERROR;
		goto out;
	}

	while (!RRR_PARSE_CHECK_EOF(pos)) {
		__rrr_array_tree_parse_definition_node_check_end(
				pos,
				&eof_found,
				semicolon_found,
				&comma_found,
				&branch_found
		);

		if (eof_found || *semicolon_found || branch_found || comma_found) {
			break;
		}

		int start;
		int end;

		rrr_parse_match_until (
				pos,
				&start,
				&end,
				RRR_PARSE_MATCH_COMMAS|RRR_PARSE_MATCH_SPACE_TAB|RRR_PARSE_MATCH_NEWLINES
		);

		if (end < start) {
			break;
		}
		else if (end == start) {
			RRR_MSG_0("Array value definition was too short (only 1 character long)\n");
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out_destroy;
		}
		else if (end - start > 64) {
			RRR_MSG_0("Array value definition was too long (more than 64 characters long)\n");
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out_destroy;
		}

		size_t length = end - start + 1; // +1 here is not for the \0

		char tmp[length + 1];
		memcpy(tmp, pos->data + start, length);
		tmp[length] = '\0';

//		printf("tmp: %s\n", tmp);

		int i; // DO NOT use unsigned
		for (i = length - 1; i >= 0; i--) {
			if (tmp[i] == ' ' || tmp[i] == '\t' || tmp[i] == '\n' || tmp[i] == '\r') {
				tmp[i] = '\0';
			}
			else {
				break;
			}
		}

		if (i < 2) {
			RRR_MSG_0("Array value definition was too short (less than 2 characters long)\n");
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out_destroy;
		}

		if ((ret = __rrr_array_tree_parse_single_definition(&node->array, tmp, tmp + length)) != 0) {
			goto out_destroy;
		}

		__rrr_array_tree_parse_definition_node_check_end (
				pos,
				&eof_found,
				semicolon_found,
				&comma_found,
				&branch_found
		);

		if (eof_found || *semicolon_found || branch_found) {
			break;
		}

		if (!comma_found && !(*semicolon_found)) {
			RRR_MSG_0("Comma or semicolon not found while parsing array definition\n");
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out;
		}
	}

	*target = node;

	goto out;
	out_destroy:
		__rrr_array_node_destroy(node);
	out:
		return ret;
}

int rrr_array_tree_parse (
		struct rrr_array_tree **target,
		struct rrr_parse_pos *pos,
		const char *name
) {
	int ret = RRR_ARRAY_TREE_OK;

	*target = NULL;

	struct rrr_array_tree *tree = NULL;
	if ((ret = rrr_array_tree_new(&tree, name)) != 0) {
		goto out;
	}

	int semicolon_found = 0;
	while (!RRR_PARSE_CHECK_EOF(pos)) {
		rrr_parse_ignore_spaces_and_increment_line(pos);
		if (RRR_PARSE_CHECK_EOF(pos)) {
			break;
		}

		if (*(pos->data + pos->pos) == ';') {
			pos->pos++;
			semicolon_found = 1;
			break;
		}

		rrr_parse_ignore_spaces_and_increment_line(pos);
		if (RRR_PARSE_CHECK_EOF(pos)) {
			break;
		}

//		printf("parse tree at position: >>%s\n", pos->data + pos->pos);

		if (rrr_parse_match_word(pos, "IF")) {
//			printf("Found IF\n");
			struct rrr_array_node *node;
			if ((ret = __rrr_array_tree_parse_if_node(&node, pos)) != 0) {
				goto out_destroy;
			}
			RRR_LL_APPEND(tree, node);

//			printf("parse tree at position after IF: >>%s\n", pos->data + pos->pos);

			while (!RRR_PARSE_CHECK_EOF(pos)) {
				rrr_parse_ignore_spaces_and_increment_line(pos);
				if (RRR_PARSE_CHECK_EOF(pos)) {
					break;
				}

//				printf("Check elsif\n");
				if (rrr_parse_match_word(pos, "ELSIF")) {
					struct rrr_array_branch *branch_elsif;
					if ((ret = __rrr_array_tree_parse_if(&branch_elsif, pos)) != 0) {
						goto out_destroy;
					}
					RRR_LL_APPEND(&(RRR_LL_LAST(tree)->branch_if->branches_elsif), branch_elsif);
				}
				else {
					break;
				}
			}

//			printf("Check else\n");
			if (rrr_parse_match_word(pos, "ELSE")) {
				struct rrr_array_tree *tree_else;
				if ((ret = rrr_array_tree_parse(&tree_else, pos, NULL)) != 0) {
					goto out_destroy;
				}
				RRR_LL_LAST(tree)->branch_if->tree_else = tree_else;
			}
		}

		// Start array definition node
		struct rrr_array_node *node;
		if ((ret = __rrr_array_tree_parse_definition_node(&semicolon_found, &node, pos)) != 0) {
			goto out_destroy;
		}

		printf("Node: %p, ptr_first: %p, ptr_last: %p\n", node, tree->ptr_first, tree->ptr_last);

		if (RRR_LL_COUNT(&node->array) == 0) {
			__rrr_array_node_destroy(node);
		}
		else {
			RRR_LL_APPEND(tree, node);
		}

		if (semicolon_found) {
			break;
		}
	}

	if (!semicolon_found) {
		RRR_MSG_0("Could not find terminating ; in array tree\n");
		ret = RRR_ARRAY_TREE_SOFT_ERROR;
		goto out_destroy;
	}

	*target = tree;

	goto out;
	out_destroy:
		rrr_array_tree_destroy(tree);
	out:
		if (ret != 0) {
			RRR_MSG_0("Array tree parsing failed at line %i position %i\n",
					pos->line, pos->pos - pos->line_begin_pos + 1);
		}
		return ret;
}

static void __rrr_array_tree_dump (
		const struct rrr_array_tree *tree,
		int level
);

#define MAKE_TABS						\
	char tabs[level + 1];				\
	for (int i = 0; i < level; i++) {	\
		tabs[i] = '\t';					\
	}									\
	tabs[level] = '\0'

static void __rrr_array_tree_branch_dump (
		const struct rrr_array_branch *branch,
		int level
) {
	MAKE_TABS;

	printf("%sIF (", tabs);
	rrr_condition_dump(&branch->condition);
	printf(")\n");
	__rrr_array_tree_dump(branch->array_tree, level + 1);
	RRR_LL_ITERATE_BEGIN(&branch->branches_elsif, const struct rrr_array_branch);
		printf("\n%sELSIF (", tabs);
		rrr_condition_dump(&node->condition);
		printf(")\n");
		__rrr_array_tree_dump(node->array_tree, level + 1);
	RRR_LL_ITERATE_END();
	if (branch->tree_else != NULL) {
		printf("\n%sELSE\n", tabs);
		__rrr_array_tree_dump(branch->tree_else, level + 1);
	}
}

static void __rrr_array_definition_dump (
		const struct rrr_array *array,
		int level
) {
	MAKE_TABS;

	printf("%s", tabs);

	RRR_LL_ITERATE_BEGIN(array, const struct rrr_type_value);
		if (node != RRR_LL_FIRST(array)) {
			printf(",");
		}
		printf("%s", node->definition->identifier);
		if (node->definition->max_length > 0) {
			if (node->import_length_ref != NULL) {
				printf ("{%s}", node->import_length_ref);
			}
			else {
				printf ("%u", node->import_length);
			}
		}
		if (RRR_TYPE_FLAG_IS_SIGNED(node->flags)) {
			printf("s");
		}
		if (node->element_count_ref != NULL) {
			printf("@{%s}", node->element_count_ref);
		}
		else if (node->element_count > 1) {
			printf("@%u", node->element_count);
		}
		if (node->tag != NULL && *(node->tag) != '\0') {
			printf("#%s", node->tag);
		}
	RRR_LL_ITERATE_END();
}

static void __rrr_array_tree_dump (
		const struct rrr_array_tree *tree,
		int level
) {
	MAKE_TABS;

	RRR_LL_ITERATE_BEGIN(tree, const struct rrr_array_node);
		if (node != RRR_LL_FIRST(tree)) {
			printf(",\n");
		}
		if (node->branch_if != NULL) {
			__rrr_array_tree_branch_dump(node->branch_if, level);
		}
		else {
			__rrr_array_definition_dump(&node->array, level);
		}
	RRR_LL_ITERATE_END();
	printf("\n%s;", tabs);
}

void rrr_array_tree_dump (
		const struct rrr_array_tree *tree
) {
	printf ("## ARRAY TREE DUMP BEGIN #############################\n");
	__rrr_array_tree_dump(tree, 0);
	printf ("\n## ARRAY TREE DUMP END ###############################\n");
}

struct rrr_array_reference_node {
	RRR_LL_NODE(struct rrr_array_reference_node);
	const struct rrr_type_value *value;
};

struct rrr_array_reference {
	RRR_LL_HEAD(struct rrr_array_reference_node);
};

static int __rrr_array_reference_push (
		struct rrr_array_reference *reference,
		const struct rrr_type_value *value
) {
	struct rrr_array_reference_node *new_reference_value = malloc(sizeof(*new_reference_value));
	if (new_reference_value == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_array_reference_push\n");
		return 1;
	}

	memset(new_reference_value, '\0', sizeof(*new_reference_value));

	new_reference_value->value = value;

	RRR_LL_PUSH(reference, new_reference_value);

	return 0;
}

static void __rrr_array_reference_pop (
		struct rrr_array_reference *reference
) {
	struct rrr_array_reference_node *node = RRR_LL_POP(reference);
	if (node == NULL) {
		RRR_BUG("BUG: Tried to pop off empty array reference in __rrr_array_reference_pop\n");
	}
	free(node);
}

static void __rrr_array_reference_clear (
		struct rrr_array_reference *reference
) {
	RRR_LL_DESTROY(reference, struct rrr_array_reference_node, free(node));
}

static int __rrr_array_validate_definition_reference_check_tag (
		const struct rrr_array_reference *reference,
		const struct rrr_array_reference_node *self,
		const char *tag
) {
	RRR_LL_ITERATE_BEGIN(reference, const struct rrr_array_reference_node);
		if (node == self) {
			RRR_LL_ITERATE_BREAK();
		}
		if (node->value->tag != NULL) {
			if (strcmp(node->value->tag, tag) == 0) {
				return 0;
			}
		}
	RRR_LL_ITERATE_END();

	RRR_MSG_0("Could not resolve tag {%s}, no previous array value found with this name\n", tag);

	return 1;
}

static int __rrr_array_validate_definition_reference (
		const struct rrr_array_reference *reference
) {
	int ret = 0;

	struct rrr_array_reference_node *node = RRR_LL_LAST(reference);

	if (node == NULL) {
		goto out;
	}

	if (node->value->definition->max_length == 0 &&
		node->value->definition->type != RRR_TYPE_MSG &&
		node->value->definition->type != RRR_TYPE_STR &&
		node->value->definition->type != RRR_TYPE_NSEP
	) {
		RRR_MSG_0("Type %s has dynamic size and cannot be at the end of a definition\n",
				node->value->definition->identifier);
		ret = 1;
	}

	RRR_LL_ITERATE_BEGIN(reference, const struct rrr_array_reference_node);
		const struct rrr_type_value *value = node->value;

		if (value->element_count_ref != NULL) {
			ret |= __rrr_array_validate_definition_reference_check_tag(reference, node, value->element_count_ref);
		}
		if (value->import_length_ref != NULL) {
			ret |= __rrr_array_validate_definition_reference_check_tag(reference, node, value->import_length_ref);
		}

		const struct rrr_type_value *prev_value = (prev != NULL ? prev->value : NULL);

		if (prev_value != NULL) {
			if (prev_value->definition->max_length == 0 &&
				prev_value->definition->type != RRR_TYPE_STR &&
				prev_value->definition->type != RRR_TYPE_NSEP &&
				value->definition->max_length == 0 &&
				value->definition->type != RRR_TYPE_STR
			) {
				RRR_MSG_0("Type %s cannot be followed type %s in array definition as we cannot know where the first ends, use a separator in between\n",
						prev_value->definition->identifier, value->definition->identifier);
				ret = 1;
			}
			else if (prev_value->definition->type == RRR_TYPE_FIXP) {
				if ((RRR_TYPE_IS_BLOB(value->definition->type) ||
					RRR_TYPE_IS_MSG(value->definition->type) ||
					RRR_TYPE_IS_64(value->definition->type)) && (
							!RRR_TYPE_IS_SEP(value->definition->type) &&
							!RRR_TYPE_IS_STR(value->definition->type)
					)
				) {
					RRR_MSG_0("Fixed point type cannot be followed type %s (binary data) in array definition as we cannot know where the fixed point ends if the binary data corresponds with ASCII characters, use a separator in between\n",
							value->definition->identifier);
					ret = 1;
				}
			}
		}
		prev = node;
	RRR_LL_ITERATE_END();

	out:
	return ret;
}

// 1: Check if return value from previous branch condition is FALSE or CONTINUE
// 2: Proceed into tree if current condition return is TRUE or CONTINUE
#define ITERATE_BRANCH_TREE_IF_TRUE(branch)													\
	do { if (ret & (RRR_ARRAY_TREE_CONTINUE|RRR_ARRAY_TREE_CONDITION_FALSE)) {	\
	ret = condition_callback(&branch->condition, callback_arg);								\
	if ((ret & ~(RRR_ARRAY_TREE_CONTINUE|RRR_ARRAY_TREE_CONDITION_FALSE)) != 0) {	\
		goto out;																			\
	}																						\
	if ((ret == RRR_ARRAY_TREE_CONDITION_TRUE) ||											\
		(ret & (RRR_ARRAY_TREE_CONTINUE)											\
	)) { int ret_tmp;																		\
		if ((ret_tmp = __rrr_array_tree_iterate (											\
				branch->array_tree,															\
				value_count,																\
				value_callback,																\
				condition_callback,															\
				leaf_callback,																\
				post_loop_callback,															\
				callback_arg																\
		)) != 0) {																			\
			ret = ret_tmp;																	\
			goto out;																		\
		}																					\
	}}} while(0)

static int __rrr_array_tree_iterate (
		const struct rrr_array_tree *tree,
		int value_count,
		int (*value_callback)(const struct rrr_type_value *value, void *arg),
		int (*condition_callback)(const struct rrr_condition *condition, void *arg),
		int (*leaf_callback)(void *arg),
		void (*post_loop_callback)(int value_count_orig, void *arg),
		void *callback_arg
) {
	int ret = 0;

	int value_count_orig = value_count;

	RRR_LL_ITERATE_BEGIN(tree, const struct rrr_array_node);
		const struct rrr_array *array = &node->array;
		RRR_LL_ITERATE_BEGIN(array, const struct rrr_type_value);
			value_count++;
			if ((ret = value_callback(node, callback_arg)) != 0) {
				goto out;
			}
		RRR_LL_ITERATE_END();

		if (node->branch_if != NULL) {
			const struct rrr_array_branch *branch_if = node->branch_if;

			// Needs to be set to continue and/or false to start check of first condition
			ret = RRR_ARRAY_TREE_CONTINUE;

			ITERATE_BRANCH_TREE_IF_TRUE(branch_if);

			RRR_LL_ITERATE_BEGIN(&branch_if->branches_elsif, const struct rrr_array_branch);
				ITERATE_BRANCH_TREE_IF_TRUE(node);
			RRR_LL_ITERATE_END();

			if (branch_if->tree_else != NULL && (ret & (RRR_ARRAY_TREE_CONTINUE|RRR_ARRAY_TREE_CONDITION_FALSE))) {
				int ret_tmp;
				if ((ret_tmp = __rrr_array_tree_iterate (
						branch_if->tree_else,
						value_count,
						value_callback,
						condition_callback,
						leaf_callback,
						post_loop_callback,
						callback_arg
				)) != 0) {
					ret = ret_tmp;
					goto out;
				}
			}
		}
		else if (RRR_LL_LAST(tree) == node) {
			if ((ret = leaf_callback(callback_arg)) != 0) {
				if (ret != RRR_ARRAY_TREE_CONTINUE) {
					goto out;
				}
			}
		}
	RRR_LL_ITERATE_END();

	if (post_loop_callback != NULL) {
		post_loop_callback(value_count_orig, callback_arg);
	}

	out:
	ret &= ~(RRR_ARRAY_TREE_CONDITION_FALSE|RRR_ARRAY_TREE_CONTINUE);
	return ret;
}

struct rrr_array_tree_branch_condition_validate_callback_data {
	const struct rrr_array_reference *reference;
	int result;
};

static int __rrr_array_tree_branch_condition_validate_callback (
		const struct rrr_condition_op *op,
		const char *value,
		const char *tag,
		void *arg
) {
	struct rrr_array_tree_branch_condition_validate_callback_data *callback_data = arg;

	(void)(value);
	(void)(op);

	if (tag == NULL) {
		return 0;
	}

	callback_data->result |= __rrr_array_validate_definition_reference_check_tag (
			callback_data->reference,
			NULL,
			tag
	);

	return 0;
}

struct rrr_array_tree_validate_callback_data {
	struct rrr_array_reference reference;
	int result;
};

int __rrr_array_tree_validate_value_callback (
		const struct rrr_type_value *value,
		void *arg
) {
	struct rrr_array_tree_validate_callback_data *callback_data = arg;
	int ret = 0;

	if (__rrr_array_reference_push(&callback_data->reference, value) != 0) {
		ret = RRR_ARRAY_TREE_HARD_ERROR;
	}

	return ret;
}

int __rrr_array_tree_validate_condition_callback (
		const struct rrr_condition *condition,
		void *arg
) {
	struct rrr_array_tree_validate_callback_data *callback_data = arg;
	struct rrr_array_tree_branch_condition_validate_callback_data condition_callback_data = {
			&callback_data->reference,
			0
	};

	rrr_condition_iterate (
			condition,
			__rrr_array_tree_branch_condition_validate_callback,
			&condition_callback_data
	);
	callback_data->result |= condition_callback_data.result;

	return RRR_ARRAY_TREE_CONTINUE;
}

int __rrr_array_tree_validate_leaf_callback (
		void *arg
) {
	struct rrr_array_tree_validate_callback_data *callback_data = arg;

	callback_data->result |= __rrr_array_validate_definition_reference (
			&callback_data->reference
	);

	return RRR_ARRAY_TREE_CONTINUE;
}

void __rrr_array_tree_validate_post_loop_callback (
		int value_count_orig,
		void *arg
) {
	struct rrr_array_tree_validate_callback_data *callback_data = arg;

	while (RRR_LL_COUNT(&callback_data->reference) > value_count_orig) {
		__rrr_array_reference_pop(&callback_data->reference);
	}
}

int rrr_array_tree_validate (
		const struct rrr_array_tree *tree
) {
	int ret = 0;

	struct rrr_array_tree_validate_callback_data callback_data = {0};

	ret = __rrr_array_tree_iterate (
			tree,
			0,
			__rrr_array_tree_validate_value_callback,
			__rrr_array_tree_validate_condition_callback,
			__rrr_array_tree_validate_leaf_callback,
			__rrr_array_tree_validate_post_loop_callback,
			&callback_data
	);

	__rrr_array_reference_clear(&callback_data.reference);

	return ret | callback_data.result;
}

struct rrr_array_tree_import_callback_data {
	struct rrr_array array;
	const char *pos;
	const char *end;
};

int __rrr_array_tree_import_value_callback (
		const struct rrr_type_value *value,
		void *arg
) {
	struct rrr_array_tree_import_callback_data *callback_data = arg;
	int ret = 0;

	struct rrr_type_value *new_value = NULL;
	if ((ret = rrr_type_value_clone(&new_value, value, 0)) != 0) {
		goto out;
	}
	RRR_LL_APPEND(&callback_data->array, new_value);

	rrr_length parsed_bytes = 0;
	ret = rrr_array_parse_data_into_value(new_value, &parsed_bytes, callback_data->pos, callback_data->end);
	callback_data->pos += parsed_bytes;

	out:
	return ret;
}

static int __rrr_array_tree_import_condition_name_evaluate_callback (
		uint64_t *result,
		const char *name,
		void *arg
) {
	int ret = 0;

	*result = 0;

	struct rrr_array *array_tmp = arg;

	RRR_LL_ITERATE_BEGIN_REVERSE(array_tmp, struct rrr_type_value);
		if (node->tag != NULL && strncmp(node->tag, name, node->tag_length) == 0) {
			*result = node->definition->to_64(node);
		}
	RRR_LL_ITERATE_END();

	return ret;
}

int __rrr_array_tree_import_condition_callback (
		const struct rrr_condition *condition,
		void *arg
) {
	struct rrr_array_tree_import_callback_data *callback_data = arg;

	int ret = RRR_ARRAY_TREE_CONDITION_FALSE;
	uint64_t result = 0;

	if ((ret = rrr_condition_evaluate (
			&result,
			condition,
			__rrr_array_tree_import_condition_name_evaluate_callback,
			&callback_data->array
	)) != 0) {
		goto out;
	}

	if (result) {
		ret = RRR_ARRAY_TREE_CONDITION_FALSE;
	}

	out:
	return ret;
}


int __rrr_array_tree_get_import_length_leaf_callback (
		void *arg
) {
	struct rrr_array_tree_import_callback_data *callback_data = arg;

	(void)(callback_data);

	// Nothing to do but returning 0 to signal completion

	return 0;
}
/*
static int __rrr_array_tree_get_import_length_value_callback (
		const struct rrr_type_value *value,
		void *arg
) {
	struct rrr_array_tree_import_callback_data *callback_data = arg;

	int ret = RRR_TYPE_PARSE_OK;

	if (callback_data->pos >= callback_data->end) {
		return RRR_TYPE_PARSE_INCOMPLETE;
	}

	rrr_length result = 0;
	if ((ret = value->definition->get_import_length (
			&result,
			value,
			callback_data->pos,
			callback_data->end - callback_data->pos
	)) != RRR_TYPE_PARSE_OK) {
		return ret;
	}

	callback_data->pos += result;
	callback_data->import_length += result;

	return ret;
}

int rrr_array_tree_get_import_length_from_buffer (
		struct rrr_array_read_data *array_read_data,
		ssize_t *import_length,
		const struct rrr_array_tree *tree,
		const char *buf,
		ssize_t buf_length
) {
	int ret = RRR_TYPE_PARSE_OK;

	*import_length = 0;

	struct rrr_array_tree_import_callback_data callback_data = {0};

	callback_data.pos = buf;
	callback_data.end = buf + buf_length;

	if ((ret = __rrr_array_tree_iterate (
			tree,
			0,
			__rrr_array_tree_get_import_length_value_callback,
			__rrr_array_tree_import_condition_callback,
			__rrr_array_tree_get_import_length_leaf_callback,
			NULL,
			&callback_data
	)) != 0) {
		goto out;
	}

	if (callback_data.import_length_final != 0) {
		rrr_array_clear(&array_read_data->final_array);
		RRR_LL_MERGE_AND_CLEAR_SOURCE_HEAD(&array_read_data->final_array, &callback_data.array);
		*import_length = callback_data.import_length_final;
		if (*import_length < 0) {
			*import_length = 0;
			RRR_MSG_0("Import length too long (%lu bytes) in rrr_array_tree_get_import_length_from_buffer\n",
					callback_data.import_length_final);
			ret = RRR_ARRAY_TREE_SOFT_ERROR;
			goto out;
		}
	}

	out:
	rrr_array_clear(&callback_data.array);
	return ret;
}
*/
int rrr_array_tree_clone (
		struct rrr_array_tree **target,
		const struct rrr_array_tree *source
) {
	int ret = 0;

	struct rrr_array_tree *new_tree = NULL;

	if ((ret = rrr_array_tree_new(&new_tree, source->name)) != 0) {
		goto out;
	}

	struct rrr_array_node *node_tmp = NULL;

	RRR_LL_ITERATE_BEGIN(source, const struct rrr_array_node);
		if ((ret = __rrr_array_node_clone(&node_tmp, node)) != 0) {
			goto out;
		}
		RRR_LL_PUSH(new_tree, node_tmp);
	RRR_LL_ITERATE_END();

	*target = new_tree;
	new_tree = NULL;

	out:
	if (new_tree != NULL) {
		rrr_array_tree_destroy(new_tree);
	}
	return ret;
}

int rrr_array_tree_parse_from_buffer (
		ssize_t *parsed_bytes,
		const char *buf,
		ssize_t buf_len,
		const struct rrr_array_tree *tree,
		int (*callback)(struct rrr_array *array, void *arg),
		void *callback_arg
) {
	int ret = 0;

	*parsed_bytes = 0;

	struct rrr_array_tree_import_callback_data callback_data = {0};

	callback_data.pos = buf;
	callback_data.end = buf + buf_len;

	if ((ret = __rrr_array_tree_iterate (
			tree,
			0,
			__rrr_array_tree_import_value_callback,
			__rrr_array_tree_import_condition_callback,
			__rrr_array_tree_get_import_length_leaf_callback,
			NULL,
			&callback_data
	)) != 0) {
		goto out;
	}

	if ((ret = callback(&callback_data.array, callback_arg)) != 0) {
		goto out;
	}

	*parsed_bytes = callback_data.pos - buf;

	out:
	rrr_array_clear(&callback_data.array);
	return ret;
}


/* DISABLED - Test and fix before enabling
struct rrr_array_tree_new_message_from_buffer_callback_intermediate_data {
	const char *topic;
	ssize_t topic_length;
	int (*callback)(struct rrr_msg_msg *message, void *arg);
	void *callback_arg;
};

static int __rrr_array_tree_new_message_from_buffer_callback_intermediate (
		struct rrr_array *array,
		void *arg
) {
	struct rrr_array_tree_new_message_from_buffer_callback_intermediate_data *callback_data = arg;

	int ret = 0;

	struct rrr_msg_msg *message = NULL;
	if ((ret = rrr_array_new_message_from_collection (
			&message,
			array,
			rrr_time_get_64(),
			callback_data->topic,
			callback_data->topic_length
	)) != 0) {
		RRR_MSG_0("Could not create message in __rrr_array_tree_new_message_from_buffer_callback_intermediate return was %i\n", ret);
		return 1;
	}
	RRR_FREE_IF_NOT_NULL(message);

	return callback_data->callback(message, callback_data->callback_arg);
}

int rrr_array_tree_new_message_from_buffer (
		const char *buf,
		ssize_t buf_len,
		const char *topic,
		ssize_t topic_length,
		const struct rrr_array_tree *tree,
		int (*callback)(struct rrr_msg_msg *message, void *arg),
		void *callback_arg
) {
	int ret = 0;

	struct rrr_array_tree_new_message_from_buffer_callback_intermediate_data callback_data = {
			topic,
			topic_length,
			callback,
			callback_arg
	};

	ssize_t parsed_bytes = 0;
	if ((ret = rrr_array_tree_parse_from_buffer (
			&parsed_bytes,
			buf,
			buf_len,
			tree,
			__rrr_array_tree_new_message_from_buffer_callback_intermediate,
			&callback_data
	)) != 0) {
		goto out;
	}

	out:
	return ret;
}
*/
