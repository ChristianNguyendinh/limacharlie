/*
Copyright (c) 2013. Victor M. Alvarez [plusvic@gmail.com].

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "mem.h"
#include "utils.h"
#include "yara.h"


#define MAX_TOKEN 4
#define MAX_TABLE_BASED_STATES_DEPTH 1


#ifndef min
#define min(x, y) ((x < y) ? (x) : (y))
#endif

typedef struct _QUEUE_NODE
{
  AC_STATE* value;

  struct _QUEUE_NODE*  previous;
  struct _QUEUE_NODE*  next;

} QUEUE_NODE;


typedef struct _QUEUE
{
  QUEUE_NODE* head;
  QUEUE_NODE* tail;

} QUEUE;


//
// _yr_ac_queue_push
//
// Pushes a state in a queue.
//
// Args:
//    QUEUE* queue     - The queue
//    AC_STATE* state  - The state
//
// Returns:
//    ERROR_SUCCESS if succeed or the corresponding error code otherwise.
//

int _yr_ac_queue_push(
    QUEUE* queue,
    AC_STATE* value)
{
  QUEUE_NODE* pushed_node;

  pushed_node = yr_malloc(sizeof(QUEUE_NODE));

  if (pushed_node == NULL)
    return ERROR_INSUFICIENT_MEMORY;

  pushed_node->previous = queue->tail;
  pushed_node->next = NULL;
  pushed_node->value = value;

  if (queue->tail != NULL)
    queue->tail->next = pushed_node;
  else // queue is empty
    queue->head = pushed_node;

  queue->tail = pushed_node;

  return ERROR_SUCCESS;
}


//
// _yr_ac_queue_pop
//
// Pops a state from a queue.
//
// Args:
//    QUEUE* queue     - The queue
//
// Returns:
//    Pointer to the poped state.
//

AC_STATE* _yr_ac_queue_pop(
    QUEUE* queue)
{
  AC_STATE* result;
  QUEUE_NODE* popped_node;

  if (queue->head == NULL)
    return NULL;

  popped_node = queue->head;
  queue->head = popped_node->next;

  if (queue->head)
    queue->head->previous = NULL;
  else // queue is empty
    queue->tail = NULL;

  result = popped_node->value;

  yr_free(popped_node);
  return result;
}


//
// _yr_ac_queue_is_empty
//
// Checks if a queue is empty.
//
// Args:
//    QUEUE* queue     - The queue
//
// Returns:
//    TRUE if queue is empty, FALSE otherwise.
//

int _yr_ac_queue_is_empty(
    QUEUE* queue)
{
  return queue->head == NULL;
}


AC_STATE* _yr_ac_next_child(
  AC_STATE* state,
  int64_t* iterator)
{
  int i;
  AC_TABLE_BASED_STATE* table_based_state;
  AC_LIST_BASED_STATE* list_based_state;
  AC_STATE_TRANSITION* transition;

  if (state->depth <= MAX_TABLE_BASED_STATES_DEPTH)
  {
    for (i = (int) *iterator; i < 256; i++)
    {
      table_based_state = (AC_TABLE_BASED_STATE*) state;

      if (table_based_state->transitions[i].state != NULL)
      {
        *iterator = i + 1;
        return table_based_state->transitions[i].state;
      }
    }
  }
  else
  {
    transition = (AC_STATE_TRANSITION*) *iterator;

    if (transition->next != NULL)
    {
      *iterator = (int64_t) transition->next;
      return transition->next->state;
    }
  }

  return NULL;
}


AC_STATE* _yr_ac_first_child(
  AC_STATE* state,
  int64_t* iterator)
{
  AC_LIST_BASED_STATE* list_based_state;

  if (state->depth <= MAX_TABLE_BASED_STATES_DEPTH)
  {
    *iterator = 0;
    return _yr_ac_next_child(state, iterator);
  }
  else
  {
    list_based_state = (AC_LIST_BASED_STATE*) state;

    if (list_based_state->transitions != NULL)
    {
      *iterator = (int64_t) list_based_state->transitions;
      return list_based_state->transitions->state;
    }
  }

  return NULL;
}

//
// yr_ac_next_state
//
// Given an automaton state and an input symbol, returns the new state
// after reading the input symbol.
//
// Args:
//    AC_STATE* state     - Automaton state
//    uint8_t input       - Input symbol
//
// Returns:
//   Pointer to the next automaton state.
//

inline AC_STATE* yr_ac_next_state(
    AC_STATE* state,
    uint8_t input)
{
  AC_STATE_TRANSITION* transition;

  if (state->depth <= MAX_TABLE_BASED_STATES_DEPTH)
  {
    return ((AC_TABLE_BASED_STATE*) state)->transitions[input].state;
  }
  else
  {
    transition = ((AC_LIST_BASED_STATE*) state)->transitions;

    while (transition != NULL)
    {
      if (transition->input == input)
        return transition->state;

      transition = transition->next;
    }

    return NULL;
  }
}


//
// _yr_ac_create_state
//
// Creates a new automaton state, the automaton will transition from
// the given state to the new state after reading the input symbol.
//
// Args:
//   ARENA* arena     - Automaton's arena
//   AC_STATE* state  - Origin state
//   uint8_t input    - Input symbol
//
// Returns:
//   AC_STATE* pointer to the newly allocated state or NULL in case
//   of error.

AC_STATE* _yr_ac_create_state(
    ARENA* arena,
    AC_STATE* state,
    uint8_t input)
{
  int result;
  AC_STATE* new_state;
  AC_LIST_BASED_STATE* list_based_state;
  AC_TABLE_BASED_STATE* table_based_state;
  AC_STATE_TRANSITION* new_transition;

  if (state->depth < MAX_TABLE_BASED_STATES_DEPTH)
  {
    result = yr_arena_allocate_struct(
        arena,
        sizeof(AC_TABLE_BASED_STATE),
        (void**) &new_state,
        offsetof(AC_TABLE_BASED_STATE, failure),
        offsetof(AC_TABLE_BASED_STATE, matches),
        EOL);
  }
  else
  {
    result = yr_arena_allocate_struct(
        arena,
        sizeof(AC_LIST_BASED_STATE),
        (void**) &new_state,
        offsetof(AC_LIST_BASED_STATE, failure),
        offsetof(AC_LIST_BASED_STATE, matches),
        offsetof(AC_LIST_BASED_STATE, transitions),
        EOL);
  }

  if (result != ERROR_SUCCESS)
    return NULL;

  if (state->depth <= MAX_TABLE_BASED_STATES_DEPTH)
  {
    result = yr_arena_make_relocatable(
        arena,
        state,
        offsetof(AC_TABLE_BASED_STATE, transitions[input]),
        EOL);

    if (result != ERROR_SUCCESS)
      return NULL;

    table_based_state = (AC_TABLE_BASED_STATE*) state;
    table_based_state->transitions[input].state = new_state;
  }
  else
  {
    result = yr_arena_allocate_struct(
        arena,
        sizeof(AC_STATE_TRANSITION),
        (void**) &new_transition,
        offsetof(AC_STATE_TRANSITION, state),
        offsetof(AC_STATE_TRANSITION, next),
        EOL);

    if (result != ERROR_SUCCESS)
      return NULL;

    list_based_state = (AC_LIST_BASED_STATE*) state;

    new_transition->input = input;
    new_transition->state = new_state;
    new_transition->next = list_based_state->transitions;
    list_based_state->transitions = new_transition;
  }

  new_state->depth = state->depth + 1;

  return new_state;
}


//
// _yr_ac_gen_case_combinations
//
// Returns all combinations of lower and upper cases for a given token. For
// token "abc" the output would be "abc" "abC" "aBC" and so on. Resulting
// tokens are written into the output buffer in this format:
//
//  [size 1] [backtrack 1] [token 1]  ... [size N] [backtrack N] [token N] [0]
//
// Notice the zero at the end to indicate where the output ends.
//
// The caller is responsible of providing a buffer large enough to hold the
// returned tokens.
//

uint8_t* _yr_ac_gen_case_combinations(
    uint8_t* token,
    int token_length,
    int token_offset,
    int token_backtrack,
    uint8_t* output_buffer)
{
  char c;
  char* new_token;

  if (token_offset + 1 < token_length)
    output_buffer = _yr_ac_gen_case_combinations(
        token,
        token_length,
        token_offset + 1,
        token_backtrack,
        output_buffer);

  c = token[token_offset];

  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
  {
    // Write token length.
    *((int*) output_buffer) = token_length;
    output_buffer += sizeof(int);

    // Write token backtrack.
    *((int*) output_buffer) = token_backtrack;
    output_buffer += sizeof(int);

    memcpy(output_buffer, token, token_length);

    new_token = output_buffer;
    output_buffer += token_length;

    // Swap character case.
    if (c >= 'a' && c <= 'z')
      new_token[token_offset] -= 32;
    else
      new_token[token_offset] += 32;

    if (token_offset + 1 < token_length)
      output_buffer = _yr_ac_gen_case_combinations(
          new_token,
          token_length,
          token_offset + 1,
          token_backtrack,
          output_buffer);
  }

  return output_buffer;
}

//
// _yr_ac_gen_hex_tokens
//
// Generates token for a hex string. The token will be a substring of length
// up to MAX_TOKEN, generally a prefix, but not necessarily. The token can
// also be extracted from the middle of the string when the prefix is not long
// enough. The function will try to choose a token with as many distinct bytes
// as posible, avoiding tokens like 00 00 00 00 which are too common.
// For example, in the string
//
//    98 56 ?? ?? 00 00 00 00 34 EB 45 97 21
//
// the token would be 34 EB 45 97 (assuming MAX_TOKEN is 4) instead of 98 56,
// which is shorter, or 00 00 00 00 which is more homogeneous.
//

uint8_t* _yr_ac_gen_hex_tokens(
    STRING* string,
    int max_token_length,
    uint8_t* output_buffer)
{
  int inside_or = 0;
  int token_length = 0;
  int backtrack = 0;
  int unique_bytes = 0;
  int max_unique_bytes = 0;
  int candidate_token_position = 0;
  int candidate_token_length = 0;
  int candidate_token_backtrack = 0;
  int or_string_length = 0;
  int previous_or_string_length = 0;
  int string_position = 0;
  int i, j, unique;

  uint8_t* mask;
  uint8_t last[MAX_TOKEN];

  mask = string->mask;

  while (*mask != MASK_END)
  {
    if (token_length == 0)
      for (i = 0; i < max_token_length; i++)
        last[i] = string->string[string_position];

    // We entered an OR operation like (01 | 02).
    if (*mask == MASK_OR)
      inside_or = TRUE;

    // We exit from an OR operation.
    if (*mask == MASK_OR_END)
      inside_or = FALSE;

    // If non-wildcard byte and not inside an OR it could
    // be used for the token.
    if (*mask == 0xFF && !inside_or)
    {
      token_length++;
      token_length = min(token_length, max_token_length);

      last[string_position % max_token_length] = \
          string->string[string_position];

      unique_bytes = 1;

      for (i = 0; i < max_token_length - 1; i++)
      {
        unique = TRUE;
        for (j = i + 1; j < max_token_length; j++)
        {
          if (last[i] == last[j])
          {
            unique = FALSE;
            break;
          }
        }
        if (unique)
          unique_bytes++;
      }

      if (unique_bytes > max_unique_bytes ||
          token_length > candidate_token_length)
      {
        max_unique_bytes = unique_bytes;
        candidate_token_position = string_position - token_length + 1;
        candidate_token_backtrack = backtrack - token_length + 1;
        candidate_token_length = token_length;

        if (candidate_token_length == max_token_length &&
            max_unique_bytes == max_token_length)
          break;
      }
    }
    else
    {
      token_length = 0;
    }

    if (*mask != MASK_OR &&
        *mask != MASK_OR_END &&
        *mask != MASK_EXACT_SKIP &&
        *mask != MASK_RANGE_SKIP)
    {
      string_position++;

      if (inside_or)
        or_string_length++;
      else
        backtrack++;
    }

    if (*mask == MASK_EXACT_SKIP)
    {
      mask++;
      backtrack += *mask;
    }
    else if (*mask == MASK_RANGE_SKIP)
    {
      break;
    }
    else if (*mask == MASK_OR || *mask == MASK_OR_END)
    {
      if (previous_or_string_length == 0)
        previous_or_string_length = or_string_length;

      // This happens when the string contains an OR with
      // alternatives of different size like: (01 | 02 03)
      // instead of (01 | 02). In those cases the backtrack
      // value would be different for each alternative, so
      // we don't want any token past the OR.
      if (or_string_length != previous_or_string_length)
        break;

      or_string_length = 0;

      if (*mask == MASK_OR_END)
      {
        backtrack += previous_or_string_length;
        previous_or_string_length = 0;
      }
    }

    mask++;
  }

  *((int*) output_buffer) = candidate_token_length;
  output_buffer += sizeof(int);

  *((int*) output_buffer) = candidate_token_backtrack;
  output_buffer += sizeof(int);

  memcpy(
      output_buffer,
      string->string + candidate_token_position,
      candidate_token_length);

  output_buffer += candidate_token_length;

  return output_buffer;
}

//
// _yr_ac_gen_regexp_tokens
//
// Generates tokens for a regular expression.
//

uint8_t* _yr_ac_gen_regexp_tokens(
    STRING* string,
    int max_token_length,
    uint8_t* output_buffer)
{
  uint8_t token[MAX_TOKEN];
  uint8_t first_bytes[256];
  uint8_t current;
  uint8_t next;

  int first_bytes_count;
  int token_length = 0;
  int i = 0;

  if (string->string[0] == '^')
    i++;

  while (i < string->length && token_length < max_token_length)
  {
    current = string->string[i];

    if (string->length > i + 1)
      next = string->string[i + 1];
    else
      next = 0;

    if (current == '\\' && isregexescapable[next])
    {
      token[token_length] = next;
      token_length++;
      i += 2;
    }
    else if (isregexhashable[current] &&
             next != '*' && next != '{' && next != '?')
    {
      // Add current character to the token if it's hashable and the next one
      // is not a quantifier. Quantifiers can make the character optional like
      // in abc*, abc{0,N}, abc?. In all this regexps the 'c' is not required
      // to appear in a matching string.

      token[token_length] = current;
      token_length++;
      i++;
    }
    else
    {
      break;
    }
  }

  if (token_length > 0)
  {
    *((int*) output_buffer) = token_length;
    output_buffer += sizeof(int);

    *((int*) output_buffer) = 0;
    output_buffer += sizeof(int);

    memcpy(output_buffer, token, token_length);
    output_buffer += token_length;

    if (STRING_IS_NO_CASE(string))
      output_buffer = _yr_ac_gen_case_combinations(
          token,
          token_length,
          0,
          0,
          output_buffer);
  }
  else
  {
    first_bytes_count = yr_regex_get_first_bytes(&(string->re), first_bytes);

    for (i = 0; i < first_bytes_count; i++)
    {
      // Write token length.
      *((int*) output_buffer) = 1;
      output_buffer += sizeof(int);

      // Write backtrack value.
      *((int*) output_buffer) = 0;
      output_buffer += sizeof(int);

      *((uint8_t*) output_buffer) = first_bytes[i];
      output_buffer += sizeof(uint8_t);
    }
  }

  return output_buffer;
}


//
// _yr_ac_gen_tokens
//
// Returns the tokens to be added to the Aho-Corasick automaton for
// a given YARA string. Length of tokens is limited by max_token_lengh.
// Tokens are written to the output buffer in the same format used by
// _yr_ac_gen_case_combinations.
//

void _yr_ac_gen_tokens(
    STRING* string,
    int max_token_length,
    uint8_t* output_buffer)
{
  int i, j;
  int token_length;
  void* str;


  if (STRING_IS_HEX(string))
  {
    output_buffer = _yr_ac_gen_hex_tokens(
        string,
        max_token_length,
        output_buffer);
  }
  else if (STRING_IS_REGEXP(string))
  {
    output_buffer = _yr_ac_gen_regexp_tokens(
        string,
        max_token_length,
        output_buffer);
  }
  else // text string
  {
    if (STRING_IS_ASCII(string))
    {
      token_length = min(string->length, max_token_length);

      // Write token length.
      *((int*) output_buffer) = token_length;
      output_buffer += sizeof(int);

      // Write backtrack value.
      *((int*) output_buffer) = 0;
      output_buffer += sizeof(int);

      str = output_buffer;

      memcpy(output_buffer, string->string, token_length);
      output_buffer += token_length;

      if (STRING_IS_NO_CASE(string))
      {
        output_buffer = _yr_ac_gen_case_combinations(
            str,
            token_length,
            0,
            0,
            output_buffer);
      }
    }

    if (STRING_IS_WIDE(string))
    {
      token_length = min(string->length * 2, max_token_length);

      // Write token length.
      *((int*) output_buffer) = token_length;
      output_buffer += sizeof(int);

      // Write backtrack value.
      *((int*) output_buffer) = 0;
      output_buffer += sizeof(int);

      str = output_buffer;
      i = j = 0;

      while(i < token_length)
      {
        if (i % 2 == 0)
          *(((uint8_t*) output_buffer)++) = string->string[j++];
        else
          *(((uint8_t*) output_buffer)++) = 0;
        i++;
      }

      if (STRING_IS_NO_CASE(string))
      {
        output_buffer = _yr_ac_gen_case_combinations(
            str,
            token_length,
            0,
            0,
            output_buffer);
      }
    }
  }

  *((int*) output_buffer) = 0;
  output_buffer += sizeof(int);
}


//
// yr_ac_create_failure_links
//
// Create failure links for each automaton state. This function must
// be called after all the strings have been added to the automaton.
//

void yr_ac_create_failure_links(
    ARENA* arena,
    AC_AUTOMATON* automaton)
{
  int i;

  int64_t iterator;

  AC_STATE* current_state;
  AC_STATE* failure_state;
  AC_STATE* temp_state;
  AC_STATE* state;
  AC_STATE* transition_state;
  AC_STATE* root_state;
  AC_MATCH* match;

  QUEUE queue;

  queue.head = NULL;
  queue.tail = NULL;

  root_state = automaton->root;

  // Set the failure link of root state to itself.
  root_state->failure = root_state;

  // Push root's children and set their failure link to root.

  state = _yr_ac_first_child(root_state, &iterator);

  while (state != NULL)
  {
    _yr_ac_queue_push(&queue, state);
    state->failure = root_state;
    state = _yr_ac_next_child(root_state, &iterator);
  }

  // Traverse the trie in BFS order calculating the failure link
  // for each state.

  while(!_yr_ac_queue_is_empty(&queue))
  {
    current_state = _yr_ac_queue_pop(&queue);

    match = current_state->matches;

    if (match != NULL)
    {
      while (match->next != NULL)
        match = match->next;

      if (match->backtrack > 0)
        match->next = root_state->matches;
    }
    else
    {
      current_state->matches = root_state->matches;
    }

    transition_state = _yr_ac_first_child(current_state, &iterator);

    while (transition_state != NULL)
    {
      _yr_ac_queue_push(&queue, transition_state);
      failure_state = current_state->failure;

      while (1)
      {
        temp_state = yr_ac_next_state(failure_state, i);

        if (temp_state != NULL)
        {
          transition_state->failure = temp_state;

          if (transition_state->matches == NULL)
          {
            transition_state->matches = temp_state->matches;
          }
          else
          {
            match = transition_state->matches;

            while (match != NULL && match->next != NULL)
              match = match->next;

            match->next = temp_state->matches;
          }

          break;
        }
        else
        {
          if (failure_state == root_state)
          {
            transition_state->failure = root_state;
            break;
          }
          else
          {
            failure_state = failure_state->failure;
          }
        }
      } // while(1)

      transition_state = _yr_ac_next_child(current_state, &iterator);
    }

  } // while(!__yr_ac_queue_is_empty(&queue))
}


//
// yr_ac_create_automaton
//
// Creates a new automaton
//

int yr_ac_create_automaton(
    ARENA* arena,
    AC_AUTOMATON** automaton)
{
  int result;
  AC_STATE* root_state;

  result = yr_arena_allocate_struct(
      arena,
      sizeof(AC_AUTOMATON),
      (void**) automaton,
      offsetof(AC_AUTOMATON, root),
      EOL);

  if (result != ERROR_SUCCESS)
    return result;

  result = yr_arena_allocate_struct(
      arena,
      sizeof(AC_TABLE_BASED_STATE),
      (void**) &root_state,
      offsetof(AC_TABLE_BASED_STATE, failure),
      offsetof(AC_TABLE_BASED_STATE, matches),
      EOL);

  if (result != ERROR_SUCCESS)
    return result;

  (*automaton)->root = root_state;

  root_state->depth = 0;
  root_state->matches = NULL;

  return result;
}


//
// yr_ac_add_string
//
// Adds a string to the automaton.
//

int yr_ac_add_string(
    ARENA* arena,
    AC_AUTOMATON* automaton,
    STRING* string,
    int* min_token_length)
{
  int result;
  int token_length;
  int token_backtrack;
  int i;

  AC_STATE* state;
  AC_STATE* next_state;
  AC_MATCH* new_match;

  uint8_t* tokens;
  uint8_t* tokens_cursor;

  // Reserve memory to hold tokens for the string. We reserve enough memory
  // for the worst case which is a "ascii wide nocase" text string.

  tokens = yr_malloc(
      2 * (1 << MAX_TOKEN) * (2 * sizeof(int) + MAX_TOKEN) + sizeof(int));

  if (tokens == NULL)
    return ERROR_INSUFICIENT_MEMORY;

  tokens_cursor = tokens;

  // Generate all posible tokens for the string. These tokens are substrings up
  // to MAX_TOKEN bytes length, which are generally a prefix of the strings,
  // but not necessarily. For hex strings token can be extracted from the middle
  // of the string. This tokens are added to the Aho-Corasick automaton.

  _yr_ac_gen_tokens(string, MAX_TOKEN, tokens);

  token_length = *((int*) tokens_cursor);
  tokens_cursor += sizeof(int);

  if (token_length == 0)
  {
    *min_token_length = 0;

    // No token could be extracted from the string, put the string in the
    // automaton's root state. This is far from ideal, because the string will
    // be tried at every data offset during scanning.

    result = yr_arena_allocate_struct(
        arena,
        sizeof(AC_MATCH),
        (void**) &new_match,
        offsetof(AC_MATCH, string),
        offsetof(AC_MATCH, next),
        EOL);

    if (result == ERROR_SUCCESS)
    {
      new_match->backtrack = 0;
      new_match->string = string;
      new_match->next = automaton->root->matches;
      automaton->root->matches = new_match;
    }
  }
  else
  {
    // For each token create the states in the automaton.

    *min_token_length = MAX_TOKEN;

    while (token_length != 0)
    {
      if (token_length < *min_token_length)
        *min_token_length = token_length;

      state = automaton->root;

      token_backtrack = *((int*) tokens_cursor);
      tokens_cursor += sizeof(int);

      for(i = 0; i < token_length; i++)
      {
        next_state = yr_ac_next_state(
            state,
            *tokens_cursor);

        if (next_state == NULL)
        {
          next_state = _yr_ac_create_state(
              arena,
              state,
              *tokens_cursor);

          if (next_state == NULL)
          {
            yr_free(tokens);
            return ERROR_INSUFICIENT_MEMORY;
          }
        }

        state = next_state;
        tokens_cursor++;
      }

      token_length = *((int*) tokens_cursor);
      tokens_cursor += sizeof(int);

      result = yr_arena_allocate_struct(
          arena,
          sizeof(AC_MATCH),
          (void**) &new_match,
          offsetof(AC_MATCH, string),
          offsetof(AC_MATCH, next),
          EOL);

      if (result == ERROR_SUCCESS)
      {
        new_match->backtrack = state->depth + token_backtrack;
        new_match->string = string;
        new_match->next = state->matches;
        state->matches = new_match;
      }
      else
      {
        break;
      }
    }
  }

  yr_free(tokens);

  return result;
}


//
// _yr_ac_print_automaton_state
//
// Prints automaton state for debug purposes. This function is invoked by
// yr_ac_print_automaton, is not intended to be used stand-alone.
//

void _yr_ac_print_automaton_state(
  AC_STATE* state)
{
  int i;
  char* identifier;
  int64_t iterator;
  STRING* string;
  AC_MATCH* match;
  AC_STATE* child_state;

  for (i = 0; i < state->depth; i++)
    printf(" ");

  printf("%p (%d) -> %p", state, state->depth, state->failure);

  match = state->matches;

  while (match != NULL)
  {
    printf(" %s:%d", match->string->identifier, match->backtrack);
    match = match->next;
  }

  printf("\n");

  child_state = _yr_ac_first_child(state, &iterator);

  while(child_state != NULL)
  {
    _yr_ac_print_automaton_state(child_state);
    child_state = _yr_ac_next_child(state, &iterator);
  }
}

//
// yr_ac_print_automaton
//
// Prints automaton for debug purposes.
//

void yr_ac_print_automaton(AC_AUTOMATON* automaton)
{
  printf("-------------------------------------------------------\n");
  _yr_ac_print_automaton_state(automaton->root);
  printf("-------------------------------------------------------\n");
}



