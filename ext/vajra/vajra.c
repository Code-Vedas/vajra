#include "vajra.h"

VALUE rb_mVajra;

RUBY_FUNC_EXPORTED void
Init_vajra(void)
{
  rb_mVajra = rb_define_module("Vajra");
}
