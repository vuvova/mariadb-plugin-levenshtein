/* Copyright (c) 2019,2024,2025,2026 MariaDB Corporation
   Copyright (c) 2026 lefred (Frédéric Descamps)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <my_sys.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <vector>

namespace {

enum Distance_status
{
  DISTANCE_OK,
  DISTANCE_NULL,
  DISTANCE_ERROR
};

struct Distance_result
{
  Distance_status status;
  size_t distance;
  size_t left_length;
  size_t right_length;
};

/*
  Decode one character. MariaDB strings are expected to be well-formed in
  their declared character set. Treat an invalid byte as one character so an
  unexpected malformed value cannot make the loop stall.
*/
static my_wc_t next_character(const String &value, const uchar **position)
{
  const uchar *end= reinterpret_cast<const uchar *>(value.end());
  my_wc_t character;
  int length= value.charset()->mb_wc(&character, *position, end);

  if (length > 0)
    *position+= length;
  else
    character= *(*position)++;
  return character;
}

static Distance_result calculate_distance(Item **args)
{
  StringBuffer<256> left_buffer;
  StringBuffer<256> right_buffer;
  String *left= args[0]->val_str(&left_buffer);
  if (!left || args[0]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};

  String *right= args[1]->val_str(&right_buffer);
  if (!right || args[1]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};

  size_t left_length= left->numchars();
  size_t right_length= right->numchars();

  const String *shorter= left;
  const String *longer= right;
  size_t shorter_length= left_length;
  size_t longer_length= right_length;
  if (shorter_length > longer_length)
  {
    std::swap(shorter, longer);
    std::swap(shorter_length, longer_length);
  }

  if (shorter_length == 0)
    return {DISTANCE_OK, longer_length, left_length, right_length};

  if (shorter_length >
      (std::numeric_limits<size_t>::max() / sizeof(size_t)) - 1)
    return {DISTANCE_ERROR, 0, left_length, right_length};

  size_t *row= static_cast<size_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, (shorter_length + 1) * sizeof(*row),
                MYF(MY_WME)));
  my_wc_t *characters= static_cast<my_wc_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, shorter_length * sizeof(*characters),
                MYF(MY_WME)));
  if (!row || !characters)
  {
    my_free(row);
    my_free(characters);
    return {DISTANCE_ERROR, 0, left_length, right_length};
  }

  const uchar *short_position=
      reinterpret_cast<const uchar *>(shorter->ptr());
  for (size_t column= 0; column < shorter_length; ++column)
  {
    characters[column]= next_character(*shorter, &short_position);
    row[column + 1]= column + 1;
  }
  row[0]= 0;

  const uchar *long_position=
      reinterpret_cast<const uchar *>(longer->ptr());
  for (size_t line= 1; line <= longer_length; ++line)
  {
    my_wc_t long_character= next_character(*longer, &long_position);
    size_t diagonal= row[0];
    row[0]= line;

    for (size_t column= 1; column <= shorter_length; ++column)
    {
      size_t above= row[column];
      size_t substitution= diagonal +
          (long_character == characters[column - 1] ? 0 : 1);
      row[column]= std::min(std::min(row[column - 1] + 1, above + 1),
                           substitution);
      diagonal= above;
    }
  }

  size_t distance= row[shorter_length];
  my_free(row);
  my_free(characters);
  return {DISTANCE_OK, distance, left_length, right_length};
}

static bool decode_string(const String &value, std::vector<my_wc_t> *output)
{
  try
  {
    output->resize(value.numchars());
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME),
             value.numchars() * sizeof(my_wc_t));
    return true;
  }

  const uchar *position= reinterpret_cast<const uchar *>(value.ptr());
  for (size_t index= 0; index < output->size(); ++index)
    (*output)[index]= next_character(value, &position);
  return false;
}

static Distance_result calculate_limited_distance(Item **args,
                                                  size_t limit)
{
  StringBuffer<256> left_buffer;
  StringBuffer<256> right_buffer;
  String *left= args[0]->val_str(&left_buffer);
  if (!left || args[0]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};
  String *right= args[1]->val_str(&right_buffer);
  if (!right || args[1]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};

  std::vector<my_wc_t> left_chars;
  std::vector<my_wc_t> right_chars;
  if (decode_string(*left, &left_chars) || decode_string(*right, &right_chars))
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};

  if (left_chars.size() > right_chars.size())
    left_chars.swap(right_chars);
  const size_t shorter_length= left_chars.size();
  const size_t longer_length= right_chars.size();
  const size_t exceeded= limit + 1;

  if (shorter_length == 0)
    return {DISTANCE_OK, longer_length <= limit ? longer_length : exceeded,
            left->numchars(), right->numchars()};

  if (longer_length - shorter_length > limit)
    return {DISTANCE_OK, exceeded, left->numchars(), right->numchars()};

  try
  {
    std::vector<size_t> previous(shorter_length + 1, exceeded);
    std::vector<size_t> current(shorter_length + 1, exceeded);
    for (size_t column= 0;
         column <= std::min(shorter_length, limit); ++column)
      previous[column]= column;

    for (size_t line= 1; line <= longer_length; ++line)
    {
      std::fill(current.begin(), current.end(), exceeded);
      if (line <= limit)
        current[0]= line;
      size_t first= line > limit ? line - limit : 1;
      size_t last= std::min(shorter_length, line + limit);
      size_t row_minimum= exceeded;
      for (size_t column= first; column <= last; ++column)
      {
        size_t deletion= previous[column] < exceeded
            ? previous[column] + 1 : exceeded;
        size_t insertion= current[column - 1] < exceeded
            ? current[column - 1] + 1 : exceeded;
        size_t substitution= previous[column - 1] +
            (right_chars[line - 1] == left_chars[column - 1] ? 0 : 1);
        current[column]=
            std::min(exceeded,
                     std::min(std::min(deletion, insertion), substitution));
        row_minimum= std::min(row_minimum, current[column]);
      }
      if (row_minimum > limit)
        return {DISTANCE_OK, exceeded, left->numchars(), right->numchars()};
      previous.swap(current);
    }
    return {DISTANCE_OK, previous[shorter_length],
            left->numchars(), right->numchars()};
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME),
             2 * (shorter_length + 1) * sizeof(size_t));
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};
  }
}

static Distance_result calculate_damerau_distance(Item **args)
{
  StringBuffer<256> left_buffer;
  StringBuffer<256> right_buffer;
  String *left= args[0]->val_str(&left_buffer);
  if (!left || args[0]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};
  String *right= args[1]->val_str(&right_buffer);
  if (!right || args[1]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};

  std::vector<my_wc_t> a;
  std::vector<my_wc_t> b;
  if (decode_string(*left, &a) || decode_string(*right, &b))
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};
  if (a.size() > b.size())
    a.swap(b);

  try
  {
    std::vector<size_t> two_back(a.size() + 1);
    std::vector<size_t> previous(a.size() + 1);
    std::vector<size_t> current(a.size() + 1);
    for (size_t column= 0; column <= a.size(); ++column)
      previous[column]= column;

    for (size_t line= 1; line <= b.size(); ++line)
    {
      current[0]= line;
      for (size_t column= 1; column <= a.size(); ++column)
      {
        current[column]= std::min(
            std::min(previous[column] + 1, current[column - 1] + 1),
            previous[column - 1] +
                (a[column - 1] == b[line - 1] ? 0 : 1));
        if (line > 1 && column > 1 &&
            a[column - 1] == b[line - 2] &&
            a[column - 2] == b[line - 1])
          current[column]= std::min(current[column],
                                    two_back[column - 2] + 1);
      }
      two_back.swap(previous);
      previous.swap(current);
    }
    return {DISTANCE_OK, previous[a.size()],
            left->numchars(), right->numchars()};
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME),
             3 * (a.size() + 1) * sizeof(size_t));
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};
  }
}

static ulonglong saturated_add(ulonglong value, ulonglong cost)
{
  const ulonglong maximum=
      static_cast<ulonglong>(std::numeric_limits<longlong>::max());
  return value > maximum - cost ? maximum : value + cost;
}

static Distance_result calculate_weighted_distance(Item **args,
                                                   ulonglong insertion_cost,
                                                   ulonglong deletion_cost,
                                                   ulonglong substitution_cost)
{
  StringBuffer<256> left_buffer;
  StringBuffer<256> right_buffer;
  String *left= args[0]->val_str(&left_buffer);
  if (!left || args[0]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};
  String *right= args[1]->val_str(&right_buffer);
  if (!right || args[1]->null_value)
    return {DISTANCE_NULL, 0, 0, 0};

  std::vector<my_wc_t> a;
  std::vector<my_wc_t> b;
  if (decode_string(*left, &a) || decode_string(*right, &b))
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};
  if (b.size() > a.size())
  {
    a.swap(b);
    std::swap(insertion_cost, deletion_cost);
  }

  try
  {
    std::vector<ulonglong> row(b.size() + 1);
    for (size_t column= 1; column <= b.size(); ++column)
      row[column]= saturated_add(row[column - 1], insertion_cost);
    for (size_t line= 1; line <= a.size(); ++line)
    {
      ulonglong diagonal= row[0];
      row[0]= saturated_add(row[0], deletion_cost);
      for (size_t column= 1; column <= b.size(); ++column)
      {
        ulonglong above= row[column];
        ulonglong substitution= saturated_add(
            diagonal, a[line - 1] == b[column - 1] ? 0 : substitution_cost);
        row[column]= std::min(
            std::min(saturated_add(above, deletion_cost),
                     saturated_add(row[column - 1], insertion_cost)),
            substitution);
        diagonal= above;
      }
    }
    return {DISTANCE_OK, static_cast<size_t>(row[b.size()]),
            left->numchars(), right->numchars()};
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME),
             (b.size() + 1) * sizeof(ulonglong));
    return {DISTANCE_ERROR, 0, left->numchars(), right->numchars()};
  }
}

template <typename Item_type, uint Argument_count>
class Create_levenshtein_function : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name,
                      List<Item> *item_list) override
  {
    uint argument_count= item_list ? item_list->elements : 0;
    if (argument_count != Argument_count)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return nullptr;
    }
    return new (thd->mem_root) Item_type(thd, *item_list);
  }
};

class Item_func_levenshtein : public Item_longlong_func
{
  using Self= Item_func_levenshtein;

public:
  using Item_longlong_func::Item_longlong_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    unsigned_flag= true;
    return Item_longlong_func::fix_length_and_dec(thd);
  }

  longlong val_int() override
  {
    Distance_result result= calculate_distance(args);
    null_value= result.status != DISTANCE_OK;
    return null_value ? 0 : static_cast<longlong>(result.distance);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_levenshtein_ratio : public Item_real_func
{
  using Self= Item_func_levenshtein_ratio;

public:
  using Item_real_func::Item_real_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    decimals= 6;
    max_length= float_length(decimals);
    return false;
  }

  double val_real() override
  {
    Distance_result result= calculate_distance(args);
    if ((null_value= result.status != DISTANCE_OK))
      return 0.0;

    size_t maximum= std::max(result.left_length, result.right_length);
    return maximum == 0
        ? 1.0
        : 1.0 - static_cast<double>(result.distance) /
                    static_cast<double>(maximum);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein_ratio"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_levenshtein_with_limit : public Item_longlong_func
{
  using Self= Item_func_levenshtein_with_limit;

public:
  using Item_longlong_func::Item_longlong_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    unsigned_flag= true;
    return Item_longlong_func::fix_length_and_dec(thd);
  }

  longlong val_int() override
  {
    longlong limit= args[2]->val_int();
    if (args[2]->null_value || limit < 0)
    {
      null_value= true;
      return false;
    }
    Distance_result result=
        calculate_limited_distance(args, static_cast<size_t>(limit));
    null_value= result.status != DISTANCE_OK;
    return null_value ? 0 : static_cast<longlong>(result.distance);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein_with_limit"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 3> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_damerau_levenshtein : public Item_longlong_func
{
  using Self= Item_func_damerau_levenshtein;

public:
  using Item_longlong_func::Item_longlong_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    unsigned_flag= true;
    return Item_longlong_func::fix_length_and_dec(thd);
  }

  longlong val_int() override
  {
    Distance_result result= calculate_damerau_distance(args);
    null_value= result.status != DISTANCE_OK;
    return null_value ? 0 : static_cast<longlong>(result.distance);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "damerau_levenshtein"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_damerau_levenshtein_ratio : public Item_real_func
{
  using Self= Item_func_damerau_levenshtein_ratio;

public:
  using Item_real_func::Item_real_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    decimals= 6;
    max_length= float_length(decimals);
    return false;
  }

  double val_real() override
  {
    Distance_result result= calculate_damerau_distance(args);
    if ((null_value= result.status != DISTANCE_OK))
      return 0.0;
    size_t maximum= std::max(result.left_length, result.right_length);
    return maximum == 0
        ? 1.0
        : 1.0 - static_cast<double>(result.distance) /
                    static_cast<double>(maximum);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "damerau_levenshtein_ratio"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_levenshtein_similar : public Item_bool_func
{
  using Self= Item_func_levenshtein_similar;

public:
  using Item_bool_func::Item_bool_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    return Item_bool_func::fix_length_and_dec(thd);
  }

  bool val_bool() override
  {
    double threshold= args[2]->val_real();
    if (args[2]->null_value || threshold < 0.0 || threshold > 1.0)
    {
      null_value= true;
      return false;
    }
    Distance_result result= calculate_distance(args);
    if ((null_value= result.status != DISTANCE_OK))
      return false;
    size_t maximum= std::max(result.left_length, result.right_length);
    double ratio= maximum == 0
        ? 1.0
        : 1.0 - static_cast<double>(result.distance) /
                    static_cast<double>(maximum);
    return ratio >= threshold;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein_similar"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 3> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_levenshtein_weighted : public Item_longlong_func
{
  using Self= Item_func_levenshtein_weighted;

public:
  using Item_longlong_func::Item_longlong_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    unsigned_flag= true;
    return Item_longlong_func::fix_length_and_dec(thd);
  }

  longlong val_int() override
  {
    longlong insertion= args[2]->val_int();
    if (args[2]->null_value)
      return null_value= true, 0;
    longlong deletion= args[3]->val_int();
    if (args[3]->null_value)
      return null_value= true, 0;
    longlong substitution= args[4]->val_int();
    if (args[4]->null_value ||
        insertion < 0 || deletion < 0 || substitution < 0)
      return null_value= true, 0;

    Distance_result result= calculate_weighted_distance(
        args, static_cast<ulonglong>(insertion),
        static_cast<ulonglong>(deletion),
        static_cast<ulonglong>(substitution));
    null_value= result.status != DISTANCE_OK;
    return null_value ? 0 : static_cast<longlong>(result.distance);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein_weighted"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 5> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

struct Edit_operation
{
  const char *name;
  size_t source_position;
  size_t destination_position;
};

class Item_func_levenshtein_editops : public Item_str_func
{
  using Self= Item_func_levenshtein_editops;

public:
  using Item_str_func::Item_str_func;

  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    max_length= MAX_BLOB_WIDTH;
    return false;
  }

  String *val_str(String *output) override
  {
    StringBuffer<256> left_buffer;
    StringBuffer<256> right_buffer;
    String *left= args[0]->val_str(&left_buffer);
    if (!left || args[0]->null_value)
      return null_value= true, nullptr;
    String *right= args[1]->val_str(&right_buffer);
    if (!right || args[1]->null_value)
      return null_value= true, nullptr;

    std::vector<my_wc_t> a;
    std::vector<my_wc_t> b;
    if (decode_string(*left, &a) || decode_string(*right, &b))
      return null_value= true, nullptr;

    try
    {
      if (a.size() != 0 &&
          b.size() + 1 > std::numeric_limits<size_t>::max() /
                           (a.size() + 1))
        return null_value= true, nullptr;
      size_t columns= b.size() + 1;
      size_t cell_count= (a.size() + 1) * columns;
      if (cell_count > std::vector<size_t>().max_size())
      {
        my_error(ER_OUTOFMEMORY, MYF(MY_WME), cell_count * sizeof(size_t));
        return null_value= true, nullptr;
      }
      std::vector<size_t> matrix(cell_count);
      for (size_t line= 0; line <= a.size(); ++line)
        matrix[line * columns]= line;
      for (size_t column= 0; column <= b.size(); ++column)
        matrix[column]= column;
      for (size_t line= 1; line <= a.size(); ++line)
      {
        for (size_t column= 1; column <= b.size(); ++column)
          matrix[line * columns + column]= std::min(
              std::min(matrix[(line - 1) * columns + column] + 1,
                       matrix[line * columns + column - 1] + 1),
              matrix[(line - 1) * columns + column - 1] +
                  (a[line - 1] == b[column - 1] ? 0 : 1));
      }

      std::vector<Edit_operation> operations;
      size_t line= a.size();
      size_t column= b.size();
      while (line != 0 || column != 0)
      {
        size_t value= matrix[line * columns + column];
        if (line != 0 && column != 0 &&
            a[line - 1] == b[column - 1] &&
            value == matrix[(line - 1) * columns + column - 1])
        {
          --line;
          --column;
        }
        else if (line != 0 && column != 0 &&
                 value == matrix[(line - 1) * columns + column - 1] + 1)
        {
          operations.push_back({"replace", line - 1, column - 1});
          --line;
          --column;
        }
        else if (column != 0 &&
                 value == matrix[line * columns + column - 1] + 1)
        {
          operations.push_back({"insert", line, column - 1});
          --column;
        }
        else
        {
          operations.push_back({"delete", line - 1, column});
          --line;
        }
      }
      std::reverse(operations.begin(), operations.end());

      output->length(0);
      output->set_charset(system_charset_info);
      if (output->append('['))
        return null_value= true, nullptr;
      for (size_t index= 0; index < operations.size(); ++index)
      {
        char buffer[160];
        const Edit_operation &operation= operations[index];
        int length= my_snprintf(
            buffer, sizeof(buffer),
            "%s{\"op\":\"%s\",\"src_pos\":%llu,\"dst_pos\":%llu}",
            index == 0 ? "" : ",", operation.name,
            static_cast<ulonglong>(operation.source_position),
            static_cast<ulonglong>(operation.destination_position));
        if (length < 0 ||
            output->append(buffer, static_cast<size_t>(length)))
          return null_value= true, nullptr;
      }
      if (output->append(']'))
        return null_value= true, nullptr;
      null_value= false;
      return output;
    }
    catch (const std::exception &)
    {
      my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
      return null_value= true, nullptr;
    }
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "levenshtein_editops"_LEX_CSTRING;
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  static Plugin_function *plugin_descriptor()
  {
    static Create_levenshtein_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

} // namespace

maria_declare_plugin(levenshtein)
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein::plugin_descriptor(),
  "levenshtein",
  "MariaDB",
  "Function LEVENSHTEIN()",
  PLUGIN_LICENSE_GPL,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein_ratio::plugin_descriptor(),
  "levenshtein_ratio",
  "MariaDB",
  "Function LEVENSHTEIN_RATIO()",
  PLUGIN_LICENSE_GPL,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein_with_limit::plugin_descriptor(),
  "levenshtein_with_limit",
  "MariaDB",
  "Function LEVENSHTEIN_WITH_LIMIT()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_damerau_levenshtein::plugin_descriptor(),
  "damerau_levenshtein",
  "MariaDB",
  "Function DAMERAU_LEVENSHTEIN()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_damerau_levenshtein_ratio::plugin_descriptor(),
  "damerau_levenshtein_ratio",
  "MariaDB",
  "Function DAMERAU_LEVENSHTEIN_RATIO()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein_similar::plugin_descriptor(),
  "levenshtein_similar",
  "MariaDB",
  "Function LEVENSHTEIN_SIMILAR()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein_editops::plugin_descriptor(),
  "levenshtein_editops",
  "MariaDB",
  "Function LEVENSHTEIN_EDITOPS()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_levenshtein_weighted::plugin_descriptor(),
  "levenshtein_weighted",
  "lefred",
  "Function LEVENSHTEIN_WEIGHTED()",
  PLUGIN_LICENSE_GPL,
  nullptr, nullptr, 0x0100, nullptr, nullptr, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
