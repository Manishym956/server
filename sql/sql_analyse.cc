/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/* Analyse database */

/* TODO: - Check if any character fields can be of any date type
**	   (date, datetime, year, time, timestamp, newdate)
**	 - Check if any number field should be a timestamp
**	 - type set is out of optimization yet
*/

#define MYSQL_LEX 1

#include "mariadb.h"
#include "sql_priv.h"
#include "procedure.h"
#include "sql_analyse.h"
#include <m_ctype.h>

#define MAX_TREEMEM	  8192
#define MAX_TREE_ELEMENTS 256

int sortcmp2(void *, const void *a_, const void *b_)
{
  const String *a= static_cast<const String*>(a_);
  const String *b= static_cast<const String*>(b_);
  return sortcmp(a,b,a->charset());
}

int compare_double2(void *, const void *s_, const void *t_)
{
  const double *s= static_cast<const double*>(s_);
  const double *t= static_cast<const double*>(t_);

  return compare_double(s,t);
}

int compare_longlong2(void *, const void *s_, const void *t_)
{
  const longlong *s= static_cast<const longlong*>(s_);
  const longlong *t= static_cast<const longlong*>(t_);
  return compare_longlong(s,t);
}

int compare_ulonglong2(void *, const void *s_, const void *t_)
{
  const ulonglong *s= static_cast<const ulonglong*>(s_);
  const ulonglong *t= static_cast<const ulonglong*>(t_);
  return compare_ulonglong(s,t);
}

int compare_decimal2(void *_len, const void *s_, const void *t_)
{
  int *len= static_cast<int *>(_len);
  const char *s= static_cast<const char *>(s_);
  const char *t= static_cast<const char *>(t_);
  return memcmp(s, t, *len);
}


static bool
prepare_param(THD *thd, Item **item, const char *proc_name, uint pos)
{
  if ((*item)->fix_fields_if_needed(thd, item))
  {
    DBUG_PRINT("info", ("fix_fields() for the parameter %u failed", pos));
    return true;
  }
  if ((*item)->type_handler()->result_type() != INT_RESULT ||
      !(*item)->basic_const_item() ||
      (*item)->val_real() < 0)
  {
    my_error(ER_WRONG_PARAMETERS_TO_PROCEDURE, MYF(0), proc_name);
    return true;
  }
  return false;
}


Procedure *
proc_analyse_init(THD *thd, ORDER *param, select_result *result,
		  List<Item> &field_list)
{
  const char *proc_name = (*param->item)->name.str;
  analyse *pc = new analyse(result);
  field_info **f_info;
  DBUG_ENTER("proc_analyse_init");

  if (!pc)
    DBUG_RETURN(0);

  if (!(param = param->next))
  {
    pc->max_tree_elements = MAX_TREE_ELEMENTS;
    pc->max_treemem = MAX_TREEMEM;
  }
  else if (param->next)
  {
    // first parameter
    if (prepare_param(thd, param->item, proc_name, 0))
      goto err;
    pc->max_tree_elements = (uint) (*param->item)->val_int();
    param = param->next;
    if (param->next)  // no third parameter possible
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_PROCEDURE, MYF(0), proc_name);
      goto err;
    }
    // second parameter
    if (prepare_param(thd, param->item, proc_name, 1))
      goto err;
    pc->max_treemem = (uint) (*param->item)->val_int();
  }
  else if (prepare_param(thd, param->item, proc_name, 0))
    goto err;
  // if only one parameter was given, it will be the value of max_tree_elements
  else
  {
    pc->max_tree_elements = (uint) (*param->item)->val_int();
    pc->max_treemem = MAX_TREEMEM;
  }

  if (!(pc->f_info= thd->alloc<field_info*>(field_list.elements)))
    goto err;
  pc->f_end = pc->f_info + field_list.elements;
  pc->fields = field_list;

  {
    List_iterator_fast<Item> it(pc->fields);
    f_info = pc->f_info;

    Item *item;
    while ((item = it++))
    {
      field_info *new_field;
      switch (item->result_type()) {
      case INT_RESULT:
        // Check if fieldtype is ulonglong
        if (item->type() == Item::FIELD_ITEM &&
            ((Item_field*) item)->field->type() == MYSQL_TYPE_LONGLONG &&
            ((Field_longlong*) ((Item_field*) item)->field)->unsigned_flag)
          new_field= new field_ulonglong(item, pc);
        else
          new_field= new field_longlong(item, pc);
        break;
      case REAL_RESULT:
        new_field= new field_real(item, pc);
        break;
      case DECIMAL_RESULT:
        new_field= new field_decimal(item, pc);
        break;
      case STRING_RESULT:
        new_field= new field_str(item, pc);
        break;
      default:
        goto err;
      }
      *f_info++= new_field;
    }
  }
  DBUG_RETURN(pc);
err:
  delete pc;
  DBUG_RETURN(0);
}


/*
  Return 1 if number, else return 0
  store info about found number in info
  NOTE:It is expected, that elements of 'info' are all zero!
*/

bool test_if_number(NUM_INFO *info, const char *str, uint str_len)
{
  const char *begin, *end= str + str_len;
  DBUG_ENTER("test_if_number");

  /*
    MySQL removes any endspaces of a string, so we must take care only of
    spaces in front of a string
  */
  for (; str != end && my_isspace(system_charset_info, *str); str++) ;
  if (str == end)
    DBUG_RETURN(0);

  if (*str == '-')
  {
    info->negative = 1;
    if (++str == end || *str == '0')    // converting -0 to a number
      DBUG_RETURN(0);                   // might lose information
  }
  else
    info->negative = 0;
  begin = str;
  for (; str != end && my_isdigit(system_charset_info,*str); str++)
  {
    if (!info->integers && *str == '0' && (str + 1) != end &&
	my_isdigit(system_charset_info,*(str + 1)))
      info->zerofill = 1;	     // could be a postnumber for example
    info->integers++;
  }
  if (str == end && info->integers)
  {
    char *endpos= (char*) end;
    int error;
    info->ullval= (ulonglong) my_strtoll10(begin, &endpos, &error);
    if (info->integers == 1)
      DBUG_RETURN(0);                   // single number can't be zerofill
    info->maybe_zerofill = 1;
    DBUG_RETURN(1);                     // a zerofill number, or an integer
  }
  if (*str == '.' || *str == 'e' || *str == 'E')
  {
    if (info->zerofill)                 // can't be zerofill anymore
      DBUG_RETURN(0);
    if ((str + 1) == end)               // number was something like '123[.eE]'
    {
      char *endpos= (char*) str;
      int error;
      info->ullval= (ulonglong) my_strtoll10(begin, &endpos, &error);
      DBUG_RETURN(1);
    }
    if (*str == 'e' || *str == 'E')     // number may be something like '1e+50'
    {
      str++;
      if (*str != '-' && *str != '+')
	DBUG_RETURN(0);
      for (str++; str != end && my_isdigit(system_charset_info,*str); str++) ;
      if (str == end)
      {
	info->is_float = 1;             // we can't use variable decimals here
	DBUG_RETURN(1);
      }
      DBUG_RETURN(0);
    }
    for (str++; *(end - 1) == '0'; end--)  // jump over zeros at the end
      ;
    if (str == end)		     // number was something like '123.000'
    {
      char *endpos= (char*) str;
      int error;
      info->ullval= (ulonglong) my_strtoll10(begin, &endpos, &error);
      DBUG_RETURN(1);
    }
    for (; str != end && my_isdigit(system_charset_info,*str); str++)
      info->decimals++;
    if (str == end)
    {
      int error;
      const char *end2= end;
      info->dval= my_strtod(begin, (char **) &end2, &error);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/*
  Stores the biggest and the smallest value from current 'info'
  to ev_num_info
  If info contains an ulonglong number, which is bigger than
  biggest positive number able to be stored in a longlong variable
  and is marked as negative, function will return 0, else 1.
*/

bool get_ev_num_info(EV_NUM_INFO *ev_info, NUM_INFO *info, const char *num)
{
  if (info->negative)
  {
    if (((longlong) info->ullval) < 0)
      return 0; // Impossible to store as a negative number
    ev_info->llval =  -(longlong) MY_MAX((ulonglong) -ev_info->llval, 
				      info->ullval);
    ev_info->min_dval = (double) -MY_MAX(-ev_info->min_dval, info->dval);
  }
  else		// ulonglong is as big as bigint in MySQL
  {
    if ((check_ulonglong(num, info->integers) == DECIMAL_NUM))
      return 0;
    ev_info->ullval = (ulonglong) MY_MAX(ev_info->ullval, info->ullval);
    ev_info->max_dval =  (double) MY_MAX(ev_info->max_dval, info->dval);
  }
  return 1;
} // get_ev_num_info


int free_string(void* str, TREE_FREE, void*)
{
  ((String*)str)->free();
  return 0;
}


void field_str::add()
{
  char buff[MAX_FIELD_WIDTH], *ptr;
  String s(buff, sizeof(buff),&my_charset_bin), *res;
  ulong length;

  if (!(res = item->val_str(&s)))
  {
    nulls++;
    return;
  }

  if (!(length = res->length()))
    empty++;
  else
  {
    ptr = (char*) res->ptr();
    if (*(ptr + (length - 1)) == ' ')
      must_be_blob = 1;
  }

  if (can_be_still_num)
  {
    bzero((char*) &num_info, sizeof(num_info));
    if (!test_if_number(&num_info, res->ptr(), (uint) length))
      can_be_still_num = 0;
    if (!found)
    {
      bzero((char*) &ev_num_info, sizeof(ev_num_info));
      was_zero_fill = num_info.zerofill;
    }
    else if (num_info.zerofill != was_zero_fill && !was_maybe_zerofill)
      can_be_still_num = 0;  // one more check needed, when length is counted
    if (can_be_still_num)
      can_be_still_num = get_ev_num_info(&ev_num_info, &num_info, res->ptr());
    was_maybe_zerofill = num_info.maybe_zerofill;
  }

  /* Update min and max arguments */
  if (!found)
  {
    found = 1;
    min_arg.copy(*res);
    max_arg.copy(*res);
    min_length = max_length = length; sum=length;
  }
  else if (length)
  {
    sum += length;
    if (length < min_length)
      min_length = length;
    if (length > max_length)
      max_length = length;

    if (sortcmp(res, &min_arg,item->collation.collation) < 0)
      min_arg.copy(*res);
    if (sortcmp(res, &max_arg,item->collation.collation) > 0)
      max_arg.copy(*res);
  }

  if (room_in_tree)
  {
    if (res != &s)
      s.copy(*res);
    if (!tree_search(&tree, (void*) &s, tree.custom_arg)) // If not in tree
    {
      s.copy();        // slow, when SAFE_MALLOC is in use
      if (!tree_insert(&tree, (void*) &s, 0, tree.custom_arg))
      {
	room_in_tree = 0;      // Remove tree, out of RAM ?
	delete_tree(&tree, 0);
      }
      else
      {
	bzero((char*) &s, sizeof(s));  // Let tree handle free of this
	if ((treemem += length) > pc->max_treemem)
	{
	  room_in_tree = 0;	 // Remove tree, too big tree
	  delete_tree(&tree, 0);
	}
      }
    }
  }

  if ((num_info.zerofill && (max_length != min_length)) ||
      (was_zero_fill && (max_length != min_length)))
    can_be_still_num = 0; // zerofilled numbers must be of same length
} // field_str::add


void field_real::add()
{
  char buff[MAX_FIELD_WIDTH], *ptr, *end;
  double num= item->val_real();
  uint length, zero_count, decs;
  TREE_ELEMENT *element;

  if (item->null_value)
  {
    nulls++;
    return;
  }
  if (num == 0.0)
    empty++;

  if ((decs = decimals()) >= FLOATING_POINT_DECIMALS)
  {
    length= snprintf(buff, sizeof(buff), "%g", num);
    if (rint(num) != num)
      max_notzero_dec_len = 1;
  }
  else
  {
    buff[sizeof(buff)-1]=0;			// Safety
    snprintf(buff, sizeof(buff)-1, "%-.*f", (int) decs, num);
    length = (uint) strlen(buff);

    // We never need to check further than this
    end = buff + length - 1 - decs + max_notzero_dec_len;

    zero_count = 0;
    for (ptr = buff + length - 1; ptr > end && *ptr == '0'; ptr--)
      zero_count++;

    if ((decs - zero_count > max_notzero_dec_len))
      max_notzero_dec_len = decs - zero_count;
  }

  if (room_in_tree)
  {
    if (!(element = tree_insert(&tree, (void*) &num, 0, tree.custom_arg)))
    {
      room_in_tree = 0;    // Remove tree, out of RAM ?
      delete_tree(&tree, 0);
    }
    /*
      if element->count == 1, this element can be found only once from tree
      if element->count == 2, or more, this element is already in tree
    */
    else if (element->count == 1 && (tree_elements++) >= pc->max_tree_elements)
    {
      room_in_tree = 0;  // Remove tree, too many elements
      delete_tree(&tree, 0);
    }
  }

  if (!found)
  {
    found = 1;
    min_arg = max_arg = sum = num;
    sum_sqr = num * num;
    min_length = max_length = length;
  }
  else if (num != 0.0)
  {
    sum += num;
    sum_sqr += num * num;
    if (length < min_length)
      min_length = length;
    if (length > max_length)
      max_length = length;
    if (compare_double(&num, &min_arg) < 0)
      min_arg = num;
    if (compare_double(&num, &max_arg) > 0)
      max_arg = num;
  }
} // field_real::add


void field_decimal::add()
{
  /*TODO - remove rounding stuff after decimal_div returns proper frac */
  VDec vdec(item);
  uint length;
  TREE_ELEMENT *element;

  if (vdec.is_null())
  {
    nulls++;
    return;
  }

  my_decimal dec;
  vdec.round_to(&dec, item->decimals, HALF_UP);

  length= my_decimal_string_length(&dec);

  if (decimal_is_zero(&dec))
    empty++;

  if (room_in_tree)
  {
    uchar buf[DECIMAL_MAX_FIELD_SIZE];
    dec.to_binary(buf, item->max_length, item->decimals);
    if (!(element = tree_insert(&tree, (void*)buf, 0, tree.custom_arg)))
    {
      room_in_tree = 0;    // Remove tree, out of RAM ?
      delete_tree(&tree, 0);
    }
    /*
      if element->count == 1, this element can be found only once from tree
      if element->count == 2, or more, this element is already in tree
    */
    else if (element->count == 1 && (tree_elements++) >= pc->max_tree_elements)
    {
      room_in_tree = 0;  // Remove tree, too many elements
      delete_tree(&tree, 0);
    }
  }

  if (!found)
  {
    found = 1;
    min_arg = max_arg = sum[0] = dec;
    my_decimal_mul(E_DEC_FATAL_ERROR, sum_sqr, &dec, &dec);
    cur_sum= 0;
    min_length = max_length = length;
  }
  else if (!decimal_is_zero(&dec))
  {
    int next_cur_sum= cur_sum ^ 1;
    my_decimal sqr_buf;

    my_decimal_add(E_DEC_FATAL_ERROR, sum+next_cur_sum, sum+cur_sum, &dec);
    my_decimal_mul(E_DEC_FATAL_ERROR, &sqr_buf, &dec, &dec);
    my_decimal_add(E_DEC_FATAL_ERROR,
                   sum_sqr+next_cur_sum, sum_sqr+cur_sum, &sqr_buf);
    cur_sum= next_cur_sum;
    if (length < min_length)
      min_length = length;
    if (length > max_length)
      max_length = length;
    if (dec.cmp(&min_arg) < 0)
    {
      min_arg= dec;
    }
    if (dec.cmp(&max_arg) > 0)
    {
      max_arg= dec;
    }
  }
}


void field_longlong::add()
{
  char buff[MAX_FIELD_WIDTH];
  longlong num = item->val_int();
  uint length = (uint) (longlong10_to_str(num, buff, -10) - buff);
  TREE_ELEMENT *element;

  if (item->null_value)
  {
    nulls++;
    return;
  }
  if (num == 0)
    empty++;

  if (room_in_tree)
  {
    if (!(element = tree_insert(&tree, (void*) &num, 0, tree.custom_arg)))
    {
      room_in_tree = 0;    // Remove tree, out of RAM ?
      delete_tree(&tree, 0);
    }
    /*
      if element->count == 1, this element can be found only once from tree
      if element->count == 2, or more, this element is already in tree
    */
    else if (element->count == 1 && (tree_elements++) >= pc->max_tree_elements)
    {
      room_in_tree = 0;  // Remove tree, too many elements
      delete_tree(&tree, 0);
    }
  }

  if (!found)
  {
    found = 1;
    min_arg = max_arg = sum = num;
    sum_sqr = num * num;
    min_length = max_length = length;
  }
  else if (num != 0)
  {
    sum += num;
    sum_sqr += num * num;
    if (length < min_length)
      min_length = length;
    if (length > max_length)
      max_length = length;
    if (compare_longlong(&num, &min_arg) < 0)
      min_arg = num;
    if (compare_longlong(&num, &max_arg) > 0)
      max_arg = num;
  }
} // field_longlong::add


void field_ulonglong::add()
{
  char buff[MAX_FIELD_WIDTH];
  longlong num = item->val_int();
  uint length = (uint) (longlong10_to_str(num, buff, 10) - buff);
  TREE_ELEMENT *element;

  if (item->null_value)
  {
    nulls++;
    return;
  }
  if (num == 0)
    empty++;

  if (room_in_tree)
  {
    if (!(element = tree_insert(&tree, (void*) &num, 0, tree.custom_arg)))
    {
      room_in_tree = 0;    // Remove tree, out of RAM ?
      delete_tree(&tree, 0);
    }
    /*
      if element->count == 1, this element can be found only once from tree
      if element->count == 2, or more, this element is already in tree
    */
    else if (element->count == 1 && (tree_elements++) >= pc->max_tree_elements)
    {
      room_in_tree = 0;  // Remove tree, too many elements
      delete_tree(&tree, 0);
    }
  }

  if (!found)
  {
    found = 1;
    min_arg = max_arg = sum = num;
    sum_sqr = num * num;
    min_length = max_length = length;
  }
  else if (num != 0)
  {
    sum += num;
    sum_sqr += num * num;
    if (length < min_length)
      min_length = length;
    if (length > max_length)
      max_length = length;
    if (compare_ulonglong((ulonglong*) &num, &min_arg) < 0)
      min_arg = num;
    if (compare_ulonglong((ulonglong*) &num, &max_arg) > 0)
      max_arg = num;
  }
} // field_ulonglong::add


int analyse::send_row(List<Item> & /* field_list */)
{
  field_info **f = f_info;

  rows++;

  for (;f != f_end; f++)
  {
    (*f)->add();
  }
  return 0;
} // analyse::send_row


int analyse::end_of_records()
{
  field_info **f = f_info;
  char buff[MAX_FIELD_WIDTH];
  String *res, s_min(buff, sizeof(buff),&my_charset_bin), 
	 s_max(buff, sizeof(buff),&my_charset_bin),
	 ans(buff, sizeof(buff),&my_charset_bin);
  StringBuffer<NAME_LEN> name;

  for (; f != f_end; f++)
  {
    /*
      We have to make a copy of full_name() as it stores it's value in str_value,
      which is reset by save_str_in_field
    */
    LEX_CSTRING col_name= (*f)->item->full_name_cstring();
    name.set_buffer_if_not_allocated(&my_charset_bin);
    name.copy(col_name.str, col_name.length, &my_charset_bin);
    func_items[0]->set((char*) name.ptr(), name.length(), &my_charset_bin);

    if (!(*f)->found)
    {
      func_items[1]->null_value = 1;
      func_items[2]->null_value = 1;
    }
    else
    {
      func_items[1]->null_value = 0;
      res = (*f)->get_min_arg(&s_min);
      func_items[1]->set(res->ptr(), res->length(), res->charset());
      func_items[2]->null_value = 0;
      res = (*f)->get_max_arg(&s_max);
      func_items[2]->set(res->ptr(), res->length(), res->charset());
    }
    func_items[3]->set((longlong) (*f)->min_length);
    func_items[4]->set((longlong) (*f)->max_length);
    func_items[5]->set((longlong) (*f)->empty);
    func_items[6]->set((longlong) (*f)->nulls);
    res = (*f)->avg(&s_max, rows);
    func_items[7]->set(res->ptr(), res->length(), res->charset());
    func_items[8]->null_value = 0;
    res = (*f)->std(&s_max, rows);
    if (!res)
      func_items[8]->null_value = 1;
    else
      func_items[8]->set(res->ptr(), res->length(), res->charset());
    /*
      count the dots, quotas, etc. in (ENUM("a","b","c"...))
      If tree has been removed, don't suggest ENUM.
      treemem is used to measure the size of tree for strings,
      tree_elements is used to count the elements
      max_treemem tells how long the string starting from ENUM("... and
      ending to ..") shall at maximum be. If case is about numbers,
      max_tree_elements will tell the length of the above, now
      every number is considered as length 1
    */
    if (((*f)->treemem || (*f)->tree_elements) &&
	(*f)->tree.elements_in_tree &&
	(((*f)->treemem ? max_treemem : max_tree_elements) >
	 (((*f)->treemem ? (*f)->treemem : (*f)->tree_elements) +
	   ((*f)->tree.elements_in_tree * 3 - 1 + 6))))
    {
      char tmp[331]; //331, because one double prec. num. can be this long
      String tmp_str(tmp, sizeof(tmp),&my_charset_bin);
      TREE_INFO tree_info;

      tree_info.str = &tmp_str;
      tree_info.found = 0;
      tree_info.item = (*f)->item;

      tmp_str.set(STRING_WITH_LEN("ENUM("),&my_charset_bin);
      tree_walk(&(*f)->tree, (*f)->collect_enum(), (char*) &tree_info,
		left_root_right);
      tmp_str.append(')');

      if (!(*f)->nulls)
	tmp_str.append(STRING_WITH_LEN(" NOT NULL"));
      output_str_length = tmp_str.length();
      func_items[9]->set(tmp_str.ptr(), tmp_str.length(), tmp_str.charset());
      if (result->send_data(result_fields) > 0)
	return -1;
      continue;
    }

    ans.length(0);
    if (!(*f)->treemem && !(*f)->tree_elements)
      ans.append(STRING_WITH_LEN("CHAR(0)"));
    else if ((*f)->item->type() == Item::FIELD_ITEM)
    {
      switch (((Item_field*) (*f)->item)->field->real_type())
      {
      case MYSQL_TYPE_TIMESTAMP:
	ans.append(STRING_WITH_LEN("TIMESTAMP"));
	break;
      case MYSQL_TYPE_DATETIME:
	ans.append(STRING_WITH_LEN("DATETIME"));
	break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
	ans.append(STRING_WITH_LEN("DATE"));
	break;
      case MYSQL_TYPE_SET:
	ans.append(STRING_WITH_LEN("SET"));
	break;
      case MYSQL_TYPE_YEAR:
	ans.append(STRING_WITH_LEN("YEAR"));
	break;
      case MYSQL_TYPE_TIME:
	ans.append(STRING_WITH_LEN("TIME"));
	break;
      case MYSQL_TYPE_DECIMAL:
	ans.append(STRING_WITH_LEN("DECIMAL"));
	// if item is FIELD_ITEM, it _must_be_ Field_num in this case
	if (((Field_num*) ((Item_field*) (*f)->item)->field)->zerofill)
	  ans.append(STRING_WITH_LEN(" ZEROFILL"));
	break;
      default:
	(*f)->get_opt_type(&ans, rows);
	break;
      }
    }
    if (!(*f)->nulls)
      ans.append(STRING_WITH_LEN(" NOT NULL"));
    func_items[9]->set(ans.ptr(), ans.length(), ans.charset());
    if (result->send_data(result_fields) > 0)
      return -1;
  }
  return 0;
} // analyse::end_of_records


void field_str::get_opt_type(String *answer, ha_rows total_rows)
{
  char buff[MAX_FIELD_WIDTH];

  if (can_be_still_num)
  {
    if (num_info.is_float)
      snprintf(buff, sizeof(buff), "DOUBLE");	  // number was like 1e+50... TODO:
    else if (num_info.decimals) // DOUBLE(%d,%d) sometime
    {
      if (num_info.dval > -FLT_MAX && num_info.dval < FLT_MAX)
	snprintf(buff, sizeof(buff), "FLOAT(%d,%d)", (num_info.integers + num_info.decimals), num_info.decimals);
      else
	snprintf(buff, sizeof(buff), "DOUBLE(%d,%d)", (num_info.integers + num_info.decimals), num_info.decimals);
    }
    else if (ev_num_info.llval >= -128 &&
	     ev_num_info.ullval <=
	     (ulonglong) (ev_num_info.llval >= 0 ? 255 : 127))
      snprintf(buff, sizeof(buff), "TINYINT(%d)", num_info.integers);
    else if (ev_num_info.llval >= INT_MIN16 &&
	     ev_num_info.ullval <= (ulonglong) (ev_num_info.llval >= 0 ?
						UINT_MAX16 : INT_MAX16))
      snprintf(buff, sizeof(buff), "SMALLINT(%d)", num_info.integers);
    else if (ev_num_info.llval >= INT_MIN24 &&
	     ev_num_info.ullval <= (ulonglong) (ev_num_info.llval >= 0 ?
						UINT_MAX24 : INT_MAX24))
      snprintf(buff, sizeof(buff), "MEDIUMINT(%d)", num_info.integers);
    else if (ev_num_info.llval >= INT_MIN32 &&
	     ev_num_info.ullval <= (ulonglong) (ev_num_info.llval >= 0 ?
						UINT_MAX32 : INT_MAX32))
      snprintf(buff, sizeof(buff), "INT(%d)", num_info.integers);
    else
      snprintf(buff, sizeof(buff), "BIGINT(%d)", num_info.integers);
    answer->append(buff, (uint) strlen(buff));
    if (ev_num_info.llval >= 0 && ev_num_info.min_dval >= 0)
      answer->append(STRING_WITH_LEN(" UNSIGNED"));
    if (num_info.zerofill)
      answer->append(STRING_WITH_LEN(" ZEROFILL"));
  }
  else if (max_length < 256)
  {
    if (must_be_blob)
    {
      if (item->collation.collation == &my_charset_bin)
	answer->append(STRING_WITH_LEN("TINYBLOB"));
      else
	answer->append(STRING_WITH_LEN("TINYTEXT"));
    }
    else if ((max_length * (total_rows - nulls)) < (sum + total_rows))
    {
      snprintf(buff, sizeof(buff), "CHAR(%d)", (int) max_length);
      answer->append(buff, (uint) strlen(buff));
    }
    else
    {
      snprintf(buff, sizeof(buff), "VARCHAR(%d)", (int) max_length);
      answer->append(buff, (uint) strlen(buff));
    }
  }
  else if (max_length < (1L << 16))
  {
    if (item->collation.collation == &my_charset_bin)
      answer->append(STRING_WITH_LEN("BLOB"));
    else
      answer->append(STRING_WITH_LEN("TEXT"));
  }
  else if (max_length < (1L << 24))
  {
    if (item->collation.collation == &my_charset_bin)
      answer->append(STRING_WITH_LEN("MEDIUMBLOB"));
    else
      answer->append(STRING_WITH_LEN("MEDIUMTEXT"));
  }
  else
  {
    if (item->collation.collation == &my_charset_bin)
      answer->append(STRING_WITH_LEN("LONGBLOB"));
    else
      answer->append(STRING_WITH_LEN("LONGTEXT"));
  }
} // field_str::get_opt_type


void field_real::get_opt_type(String *answer,
			      ha_rows total_rows __attribute__((unused)))
{
  char buff[MAX_FIELD_WIDTH];

  if (!max_notzero_dec_len)
  {
    int len= (int) max_length - ((item->decimals >= FLOATING_POINT_DECIMALS) ?
				 0 : (item->decimals + 1));

    if (min_arg >= -128 && max_arg <= (min_arg >= 0 ? 255 : 127))
      snprintf(buff, sizeof(buff), "TINYINT(%d)", len);
    else if (min_arg >= INT_MIN16 && max_arg <= (min_arg >= 0 ?
						 UINT_MAX16 : INT_MAX16))
      snprintf(buff, sizeof(buff), "SMALLINT(%d)", len);
    else if (min_arg >= INT_MIN24 && max_arg <= (min_arg >= 0 ?
						 UINT_MAX24 : INT_MAX24))
      snprintf(buff, sizeof(buff), "MEDIUMINT(%d)", len);
    else if (min_arg >= INT_MIN32 && max_arg <= (min_arg >= 0 ?
						 UINT_MAX32 : INT_MAX32))
      snprintf(buff, sizeof(buff), "INT(%d)", len);
    else
      snprintf(buff, sizeof(buff), "BIGINT(%d)", len);
    answer->append(buff, (uint) strlen(buff));
    if (min_arg >= 0)
      answer->append(STRING_WITH_LEN(" UNSIGNED"));
  }
  else if (item->decimals >= FLOATING_POINT_DECIMALS)
  {
    if (min_arg >= -FLT_MAX && max_arg <= FLT_MAX)
      answer->append(STRING_WITH_LEN("FLOAT"));
    else
      answer->append(STRING_WITH_LEN("DOUBLE"));
  }
  else
  {
    if (min_arg >= -FLT_MAX && max_arg <= FLT_MAX)
      snprintf(buff, sizeof(buff), "FLOAT(%d,%d)", (int) max_length - (item->decimals + 1) + max_notzero_dec_len,
	      max_notzero_dec_len);
    else
      snprintf(buff, sizeof(buff), "DOUBLE(%d,%d)", (int) max_length - (item->decimals + 1) + max_notzero_dec_len,
	      max_notzero_dec_len);
    answer->append(buff, (uint) strlen(buff));
  }
  // if item is FIELD_ITEM, it _must_be_ Field_num in this class
  if (item->type() == Item::FIELD_ITEM &&
      // a single number shouldn't be zerofill
      (max_length - (item->decimals + 1)) != 1 &&
      ((Field_num*) ((Item_field*) item)->field)->zerofill)
    answer->append(STRING_WITH_LEN(" ZEROFILL"));
} // field_real::get_opt_type


void field_longlong::get_opt_type(String *answer,
				  ha_rows total_rows __attribute__((unused)))
{
  char buff[MAX_FIELD_WIDTH];

  if (min_arg >= -128 && max_arg <= (min_arg >= 0 ? 255 : 127))
    snprintf(buff, sizeof(buff), "TINYINT(%d)", (int) max_length);
  else if (min_arg >= INT_MIN16 && max_arg <= (min_arg >= 0 ?
					       UINT_MAX16 : INT_MAX16))
    snprintf(buff, sizeof(buff), "SMALLINT(%d)", (int) max_length);
  else if (min_arg >= INT_MIN24 && max_arg <= (min_arg >= 0 ?
					       UINT_MAX24 : INT_MAX24))
    snprintf(buff, sizeof(buff), "MEDIUMINT(%d)", (int) max_length);
  else if (min_arg >= INT_MIN32 &&
           (ulonglong) max_arg <= (min_arg >= 0 ?
                                   (ulonglong) UINT_MAX32 :
                                   (ulonglong) INT_MAX32))
    snprintf(buff, sizeof(buff), "INT(%d)", (int) max_length);
  else
    snprintf(buff, sizeof(buff), "BIGINT(%d)", (int) max_length);
  answer->append(buff, (uint) strlen(buff));
  if (min_arg >= 0)
    answer->append(STRING_WITH_LEN(" UNSIGNED"));

  // if item is FIELD_ITEM, it _must_be_ Field_num in this class
  if ((item->type() == Item::FIELD_ITEM) &&
      // a single number shouldn't be zerofill
      max_length != 1 &&
      ((Field_num*) ((Item_field*) item)->field)->zerofill)
    answer->append(STRING_WITH_LEN(" ZEROFILL"));
} // field_longlong::get_opt_type


void field_ulonglong::get_opt_type(String *answer,
				   ha_rows total_rows __attribute__((unused)))
{
  char buff[MAX_FIELD_WIDTH];

  if (max_arg < 256)
    snprintf(buff, sizeof(buff), "TINYINT(%d) UNSIGNED", (int) max_length);
   else if (max_arg <= ((2 * INT_MAX16) + 1))
     snprintf(buff, sizeof(buff), "SMALLINT(%d) UNSIGNED", (int) max_length);
  else if (max_arg <= ((2 * INT_MAX24) + 1))
    snprintf(buff, sizeof(buff), "MEDIUMINT(%d) UNSIGNED", (int) max_length);
  else if (max_arg < (((ulonglong) 1) << 32))
    snprintf(buff, sizeof(buff), "INT(%d) UNSIGNED", (int) max_length);
  else
    snprintf(buff, sizeof(buff), "BIGINT(%d) UNSIGNED", (int) max_length);
  // if item is FIELD_ITEM, it _must_be_ Field_num in this class
  answer->append(buff, (uint) strlen(buff));
  if (item->type() == Item::FIELD_ITEM &&
      // a single number shouldn't be zerofill
      max_length != 1 &&
      ((Field_num*) ((Item_field*) item)->field)->zerofill)
    answer->append(STRING_WITH_LEN(" ZEROFILL"));
} //field_ulonglong::get_opt_type


void field_decimal::get_opt_type(String *answer,
                                 ha_rows total_rows __attribute__((unused)))
{
  my_decimal zero;
  char buff[MAX_FIELD_WIDTH];
  uint length;

  my_decimal_set_zero(&zero);
  my_bool is_unsigned= (zero.cmp(&min_arg) >= 0);

  length= snprintf(buff, sizeof(buff), "DECIMAL(%d, %d)",
                  (int) (max_length - (item->decimals ? 1 : 0)),
                  item->decimals);
  if (is_unsigned)
    length= (uint) (strmov(buff+length, " UNSIGNED")- buff);
  answer->append(buff, length);
}


String *field_decimal::get_min_arg(String *str)
{
  min_arg.to_string_native(str, 0, 0, '0');
  return str;
}


String *field_decimal::get_max_arg(String *str)
{
  max_arg.to_string_native(str, 0, 0, '0');
  return str;
}


String *field_decimal::avg(String *s, ha_rows rows)
{
  if (!(rows - nulls))
  {
    s->set_real((double) 0.0, 1,my_thd_charset);
    return s;
  }
  my_decimal num, avg_val, rounded_avg;
  int prec_increment= current_thd->variables.div_precincrement;

  int2my_decimal(E_DEC_FATAL_ERROR, rows - nulls, FALSE, &num);
  my_decimal_div(E_DEC_FATAL_ERROR, &avg_val, sum+cur_sum, &num, prec_increment);
  /* TODO remove this after decimal_div returns proper frac */
  avg_val.round_to(&rounded_avg,
                   MY_MIN(sum[cur_sum].frac + prec_increment, DECIMAL_MAX_SCALE),
                   HALF_UP);
  rounded_avg.to_string_native(s, 0, 0, '0');
  return s;
}


String *field_decimal::std(String *s, ha_rows rows)
{
  if (!(rows - nulls))
  {
    s->set_real((double) 0.0, 1,my_thd_charset);
    return s;
  }
  my_decimal num, tmp, sum2, sum2d;
  int prec_increment= current_thd->variables.div_precincrement;

  int2my_decimal(E_DEC_FATAL_ERROR, rows - nulls, FALSE, &num);
  my_decimal_mul(E_DEC_FATAL_ERROR, &sum2, sum+cur_sum, sum+cur_sum);
  my_decimal_div(E_DEC_FATAL_ERROR, &tmp, &sum2, &num, prec_increment);
  my_decimal_sub(E_DEC_FATAL_ERROR, &sum2, sum_sqr+cur_sum, &tmp);
  my_decimal_div(E_DEC_FATAL_ERROR, &tmp, &sum2, &num, prec_increment);
  double std_sqr= tmp.to_double();
  s->set_real(((double) std_sqr <= 0.0 ? 0.0 : sqrt(std_sqr)),
         MY_MIN(item->decimals + prec_increment, NOT_FIXED_DEC), my_thd_charset);

  return s;
}


int collect_string(void *element_, element_count, void *info_)
{
  String *element= static_cast<String*>(element_);
  TREE_INFO *info= static_cast<TREE_INFO*>(info_);
  if (info->found)
    info->str->append(',');
  else
    info->found = 1;
  info->str->append('\'');
  if (info->str->append_for_single_quote(element))
    return 1;
  info->str->append('\'');
  return 0;
} // collect_string


int collect_real(void *element_, element_count, void *info_)
{
  double *element= static_cast<double*>(element_);
  TREE_INFO *info= static_cast<TREE_INFO*>(info_);
  char buff[MAX_FIELD_WIDTH];
  String s(buff, sizeof(buff),current_thd->charset());

  if (info->found)
    info->str->append(',');
  else
    info->found = 1;
  info->str->append('\'');
  s.set_real(*element, info->item->decimals, current_thd->charset());
  info->str->append(s);
  info->str->append('\'');
  return 0;
} // collect_real


int collect_decimal(void *element_, element_count count, void *info_)
{
  uchar *element= static_cast<uchar*>(element_);
  TREE_INFO *info= static_cast<TREE_INFO*>(info_);
  char buff[DECIMAL_MAX_STR_LENGTH];
  String s(buff, sizeof(buff),&my_charset_bin);

  if (info->found)
    info->str->append(',');
  else
    info->found = 1;
  my_decimal dec(element, info->item->max_length, info->item->decimals);
  info->str->append('\'');
  dec.to_string_native(&s, 0, 0, '0');
  info->str->append(s);
  info->str->append('\'');
  return 0;
}


int collect_longlong(void *element_, element_count, void *info_)
{
  longlong *element= static_cast<longlong*>(element_);
  TREE_INFO *info= static_cast<TREE_INFO*>(info_);
  char buff[MAX_FIELD_WIDTH];
  String s(buff, sizeof(buff),&my_charset_bin);

  if (info->found)
    info->str->append(',');
  else
    info->found = 1;
  info->str->append('\'');
  s.set(*element, current_thd->charset());
  info->str->append(s);
  info->str->append('\'');
  return 0;
} // collect_longlong


int collect_ulonglong(void *element_, element_count, void *info_)
{
  ulonglong *element= static_cast<ulonglong*>(element_);
  TREE_INFO *info= static_cast<TREE_INFO*>(info_);
  char buff[MAX_FIELD_WIDTH];
  String s(buff, sizeof(buff),&my_charset_bin);

  if (info->found)
    info->str->append(',');
  else
    info->found = 1;
  info->str->append('\'');
  s.set(*element, current_thd->charset());
  info->str->append(s);
  info->str->append('\'');
  return 0;
} // collect_ulonglong


bool analyse::change_columns(THD *thd, List<Item> &field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;
  field_list.empty();

  func_items[0]= new (mem_root) Item_proc_string(thd, "Field_name", 255);
  func_items[1]= new (mem_root) Item_proc_string(thd, "Min_value", 255);
  func_items[1]->set_maybe_null();
  func_items[2]= new (mem_root) Item_proc_string(thd, "Max_value", 255);
  func_items[2]->set_maybe_null();
  func_items[3]= new (mem_root) Item_proc_int(thd, "Min_length");
  func_items[4]= new (mem_root) Item_proc_int(thd, "Max_length");
  func_items[5]= new (mem_root) Item_proc_int(thd, "Empties_or_zeros");
  func_items[6]= new (mem_root) Item_proc_int(thd, "Nulls");
  func_items[7]= new (mem_root) Item_proc_string(thd, "Avg_value_or_avg_length", 255);
  func_items[8]= new (mem_root) Item_proc_string(thd, "Std", 255);
  func_items[8]->set_maybe_null();
  func_items[9]= new (mem_root) Item_proc_string(thd, "Optimal_fieldtype",
                                                  MY_MAX(64,
                                                         output_str_length));

  for (uint i = 0; i < array_elements(func_items); i++)
    field_list.push_back(func_items[i], thd->mem_root);
  result_fields = field_list;
  return 0;
} // analyse::change_columns

int compare_double(const double *s, const double *t)
{
  return ((*s < *t) ? -1 : *s > *t ? 1 : 0);
} /* compare_double */

int compare_longlong(const longlong *s, const longlong *t)
{
  return ((*s < *t) ? -1 : *s > *t ? 1 : 0);
} /* compare_longlong */

 int compare_ulonglong(const ulonglong *s, const ulonglong *t)
{
  return ((*s < *t) ? -1 : *s > *t ? 1 : 0);
} /* compare_ulonglong */


uint check_ulonglong(const char *str, uint length)
{
  const char *long_str = "2147483647", *ulonglong_str = "18446744073709551615";
  const uint long_len = 10, ulonglong_len = 20;

  while (length && *str == '0')
  {
    str++; length--;
  }
  if (length < long_len)
    return NUM;

  uint smaller, bigger;
  const char *cmp;

  if (length == long_len)
  {
    cmp = long_str;
    smaller = NUM;
    bigger = LONG_NUM;
  }
  else if (length > ulonglong_len)
    return DECIMAL_NUM;
  else
  {
    cmp = ulonglong_str;
    smaller = LONG_NUM;
    bigger = DECIMAL_NUM;
  }
  while (*cmp && *cmp++ == *str++) ;
  return ((uchar) str[-1] <= (uchar) cmp[-1]) ? smaller : bigger;
} /* check_ulonglong */
