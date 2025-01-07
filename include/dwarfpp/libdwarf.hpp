#ifndef DWARFPP_LIBDWARF_HPP_
#define DWARFPP_LIBDWARF_HPP_

#include <iostream>
#include <libelf.h>
#include "config.h" /* our configure-generated header, for HAVE_DWARF_FRAME_OP3 */

namespace dwarf
{
	namespace lib
	{
		//using namespace ::dwarf::spec;
		extern "C"
		{
			// HACK: libdwarf.h declares struct Elf opaquely, and we don't
			// want it in the dwarf::lib namespace, so preprocess this.
			// FIXME: this should be #ifdef'd to affect only DA's libdwarf
			#define Elf Elf_opaque_in_libdwarf
			#include "dwarf-lib.h"
			#undef Elf
		}
		// forward decls
		struct loclist;
		
		// we need this soon
		/* The point of this class is to make a self-contained throwable object
		 * out of a Dwarf_Error handle. FIXME: why do we bundle the Ptr but not the
		 * error function? */
		struct Error {
			Dwarf_Error e;
			Dwarf_Ptr arg;
			Error(Dwarf_Error e, Dwarf_Ptr arg) : e(e), arg(arg) {}
			/*virtual*/ ~Error() 
			{ dwarf_dealloc((Dwarf_Debug) arg, e, DW_DLA_ERROR);
			  /* TODO: Fix segfault here */
			  /* Could it be because we're "catching a polymorphic type by value"?
			   * I think this is a case of a non-virtual destructor being sensible. */
			}
		};
		struct No_entry {
			No_entry() {}
		};	
		void default_error_handler(Dwarf_Error error, Dwarf_Ptr errarg); 

		bool operator==(const Dwarf_Ranges& e1, const Dwarf_Ranges& e2);
		bool operator!=(const Dwarf_Ranges& e1, const Dwarf_Ranges& e2);
		std::ostream& operator<<(std::ostream& s, const Dwarf_Ranges& e);
		bool operator==(const Dwarf_Loc& e1, const Dwarf_Loc& e2);
		bool operator!=(const Dwarf_Loc& e1, const Dwarf_Loc& e2);
		bool operator<(const lib::Dwarf_Loc& arg1, const lib::Dwarf_Loc& arg2);
		std::ostream& operator<<(std::ostream& s, const Dwarf_Loc /* a.k.a. expr_instr */ & e);
		std::ostream& operator<<(std::ostream& s, const Dwarf_Locdesc& ld);
		bool operator==(const Dwarf_Ranges& e1, const Dwarf_Ranges& e2);
		bool operator!=(const Dwarf_Ranges& e1, const Dwarf_Ranges& e2);
		std::ostream& operator<<(std::ostream& s, const Dwarf_Ranges& e);

#if HAVE_DWARF_FRAME_OP3
		/* Avoid introducing a new/distinct type unnecessarily. */
		typedef Dwarf_Frame_Op3 frame_op;
#else
		struct frame_op {
			Dwarf_Small fp_base_op;
			Dwarf_Small fp_extended_op;
			Dwarf_Half fp_register;
			Dwarf_Unsigned fp_offset_or_block_len;
			Dwarf_Small *fp_expr_block;
			Dwarf_Off fp_instr_offset;
		};
#endif
	}
}

#endif
