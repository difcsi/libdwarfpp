/* dwarfpp: C++ binding for a useful subset of libdwarf, plus extra goodies.
 *
 * libdw-compat.cpp: a libdwarf-compatible C API implemented over elfutils'
 * libdw. See include/dwarfpp/libdw.hpp for the rationale.
 *
 * Calls into libdw itself are written with a leading "::" so that, inside
 * namespace dwarf::lib, they resolve to elfutils' functions rather than to
 * the compatibility wrappers of the same name defined here.
 *
 * Copyright (c) 2008--26, Stephen Kell. For licensing information, see the
 * LICENSE file in the root of the libdwarfpp tree.
 */

#include "dwarfpp/libdw.hpp"
#include "dwarfpp/util.hpp"

/* The real elfutils types/functions live here only, kept out of the public
 * header (see libdw.hpp). */
extern "C" {
#include <elfutils/libdw.h>
}

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <map>
#include <gelf.h>

namespace dwarf
{
	namespace lib
	{
		/* Definitions of the opaque handle structs forward-declared in libdw.hpp.
		 * Each wraps a copy of the corresponding libdw by-value struct. */
		struct Dwarf_Debug_s
		{
			::Dwarf *dw;
			::Elf *elf;            // non-null only if we opened it ourselves
			int fd;                // -1 unless we opened it
			/* state for emulating libdwarf's stateful CU cursor */
			::Dwarf_Off next_cu_off;        // arg for the next dwarf_next_unit
			::Dwarf_Off current_cu_die_off; // DIE offset of the current CU
			bool have_current;
		};
		struct Dwarf_Die_s
		{
			::Dwarf_Die d;
			Dwarf_Debug_s *dbg;
		};
		struct Dwarf_Attribute_s
		{
			::Dwarf_Attribute a;
			Dwarf_Debug_s *dbg;
		};
		struct Dwarf_Error_s { int errnum; };

		/* NB: default_error_handler(), the thread-local current_dwarf_error and
		 * exception_error_handler() are defined in the backend-neutral
		 * libdwarf.cpp, which is compiled for both backends. */

		/* ---- session / sections ----------------------------------------- */
		int dwarf_init(int fd, int /*access*/, Dwarf_Handler /*errhand*/,
			Dwarf_Ptr /*errarg*/, Dwarf_Debug *ret_dbg, Dwarf_Error * /*error*/)
		{
			::Dwarf *dw = ::dwarf_begin(fd, DWARF_C_READ);
			if (!dw) return DW_DLV_ERROR;
			Dwarf_Debug dbg = new Dwarf_Debug_s();
			dbg->dw = dw; dbg->elf = nullptr; dbg->fd = -1;
			dbg->next_cu_off = 0; dbg->current_cu_die_off = 0; dbg->have_current = false;
			*ret_dbg = dbg;
			return DW_DLV_OK;
		}

		int dwarf_elf_init(Elf_opaque_in_libdwarf *elf, int /*access*/,
			Dwarf_Handler /*errhand*/, Dwarf_Ptr /*errarg*/,
			Dwarf_Debug *ret_dbg, Dwarf_Error * /*error*/)
		{
			::Dwarf *dw = ::dwarf_begin_elf(reinterpret_cast< ::Elf*>(elf),
				DWARF_C_READ, nullptr);
			if (!dw) return DW_DLV_ERROR;
			Dwarf_Debug dbg = new Dwarf_Debug_s();
			dbg->dw = dw; dbg->elf = nullptr; dbg->fd = -1;
			dbg->next_cu_off = 0; dbg->current_cu_die_off = 0; dbg->have_current = false;
			*ret_dbg = dbg;
			return DW_DLV_OK;
		}

		int dwarf_finish(Dwarf_Debug dbg, Dwarf_Error * /*error*/)
		{
			if (dbg)
			{
				if (dbg->dw) ::dwarf_end(dbg->dw);
				delete dbg;
			}
			return DW_DLV_OK;
		}

		int dwarf_get_elf(Dwarf_Debug dbg, Elf_opaque_in_libdwarf **elf,
			Dwarf_Error * /*error*/)
		{
			::Elf *e = ::dwarf_getelf(dbg->dw);
			if (!e) return DW_DLV_ERROR;
			*elf = reinterpret_cast<Elf_opaque_in_libdwarf*>(e);
			return DW_DLV_OK;
		}

		/* ---- CU / DIE traversal ----------------------------------------- *
		 * libdwarf exposes a stateful cursor: dwarf_next_cu_header_b() steps
		 * to the next CU and dwarf_siblingof(dbg, NULL, ...) yields the root
		 * DIE of the *current* CU. We emulate that state inside Dwarf_Debug_s.
		 */
		int dwarf_next_cu_header_b(Dwarf_Debug dbg,
			Dwarf_Unsigned *cu_header_length, Dwarf_Half *version_stamp,
			Dwarf_Unsigned *abbrev_offset, Dwarf_Half *address_size,
			Dwarf_Half *offset_size, Dwarf_Half *extension_size,
			Dwarf_Unsigned *next_cu_header_offset, Dwarf_Error * /*error*/)
		{
			::Dwarf_Off next_off = 0;
			size_t header_size = 0;
			::Dwarf_Half version = 0;
			::Dwarf_Off abbrev_off = 0;
			uint8_t addr_size = 0, off_size = 0;
			int r = ::dwarf_next_unit(dbg->dw, dbg->next_cu_off, &next_off,
				&header_size, &version, &abbrev_off, &addr_size, &off_size,
				nullptr, nullptr);
			if (r != 0)
			{
				/* r > 0 means no more units; r < 0 means error. Either way we
				 * reset the cursor, matching libdwarf's wrap-around behaviour. */
				dbg->next_cu_off = 0;
				dbg->current_cu_die_off = 0;
				dbg->have_current = false;
				return (r < 0) ? DW_DLV_ERROR : DW_DLV_NO_ENTRY;
			}
			dbg->current_cu_die_off = dbg->next_cu_off + header_size;
			dbg->have_current = true;
			if (cu_header_length)  *cu_header_length  = header_size;
			if (version_stamp)     *version_stamp     = version;
			if (abbrev_offset)     *abbrev_offset     = abbrev_off;
			if (address_size)      *address_size      = addr_size;
			if (offset_size)       *offset_size       = off_size;
			if (extension_size)    *extension_size    = 0;
			if (next_cu_header_offset) *next_cu_header_offset = next_off;
			dbg->next_cu_off = next_off;
			return DW_DLV_OK;
		}

		int dwarf_siblingof(Dwarf_Debug dbg, Dwarf_Die die,
			Dwarf_Die *ret_sib, Dwarf_Error * /*error*/)
		{
			if (!die)
			{
				/* "first DIE of the current CU" */
				if (!dbg->have_current) return DW_DLV_NO_ENTRY;
				::Dwarf_Die cu;
				if (::dwarf_offdie(dbg->dw, dbg->current_cu_die_off, &cu) == nullptr)
					return DW_DLV_ERROR;
				Dwarf_Die out = new Dwarf_Die_s();
				out->d = cu; out->dbg = dbg;
				*ret_sib = out;
				return DW_DLV_OK;
			}
			::Dwarf_Die sib;
			int r = ::dwarf_siblingof(&die->d, &sib);
			if (r == 0)
			{
				Dwarf_Die out = new Dwarf_Die_s();
				out->d = sib; out->dbg = die->dbg;
				*ret_sib = out;
				return DW_DLV_OK;
			}
			return (r < 0) ? DW_DLV_ERROR : DW_DLV_NO_ENTRY;
		}

		int dwarf_child(Dwarf_Die die, Dwarf_Die *ret_child, Dwarf_Error * /*error*/)
		{
			::Dwarf_Die kid;
			int r = ::dwarf_child(&die->d, &kid);
			if (r == 0)
			{
				Dwarf_Die out = new Dwarf_Die_s();
				out->d = kid; out->dbg = die->dbg;
				*ret_child = out;
				return DW_DLV_OK;
			}
			return (r < 0) ? DW_DLV_ERROR : DW_DLV_NO_ENTRY;
		}

		int dwarf_offdie(Dwarf_Debug dbg, Dwarf_Off off, Dwarf_Die *ret_die,
			Dwarf_Error * /*error*/)
		{
			::Dwarf_Die d;
			if (::dwarf_offdie(dbg->dw, off, &d) == nullptr) return DW_DLV_ERROR;
			Dwarf_Die out = new Dwarf_Die_s();
			out->d = d; out->dbg = dbg;
			*ret_die = out;
			return DW_DLV_OK;
		}

		/* ---- DIE accessors ---------------------------------------------- */
		int dwarf_dieoffset(Dwarf_Die die, Dwarf_Off *ret_off, Dwarf_Error * /*error*/)
		{
			*ret_off = ::dwarf_dieoffset(&die->d);
			return DW_DLV_OK;
		}

		int dwarf_tag(Dwarf_Die die, Dwarf_Half *ret_tag, Dwarf_Error * /*error*/)
		{
			*ret_tag = (Dwarf_Half) ::dwarf_tag(&die->d);
			return DW_DLV_OK;
		}

		int dwarf_diename(Dwarf_Die die, char **ret_name, Dwarf_Error * /*error*/)
		{
			const char *n = ::dwarf_diename(&die->d);
			if (!n) return DW_DLV_NO_ENTRY;
			*ret_name = const_cast<char*>(n);
			return DW_DLV_OK;
		}

		int dwarf_hasattr(Dwarf_Die die, Dwarf_Half attr, Dwarf_Bool *ret_bool,
			Dwarf_Error * /*error*/)
		{
			*ret_bool = ::dwarf_hasattr(&die->d, attr) ? 1 : 0;
			return DW_DLV_OK;
		}

		int dwarf_CU_dieoffset_given_die(Dwarf_Die die, Dwarf_Off *ret_off,
			Dwarf_Error * /*error*/)
		{
			::Dwarf_Die cu;
			if (::dwarf_diecu(&die->d, &cu, nullptr, nullptr) == nullptr)
				return DW_DLV_ERROR;
			*ret_off = ::dwarf_dieoffset(&cu);
			return DW_DLV_OK;
		}

		/* ---- attribute access ------------------------------------------- */
		int dwarf_attr(Dwarf_Die die, Dwarf_Half attr, Dwarf_Attribute *ret_attr,
			Dwarf_Error * /*error*/)
		{
			::Dwarf_Attribute a;
			if (::dwarf_attr(&die->d, attr, &a) == nullptr) return DW_DLV_NO_ENTRY;
			Dwarf_Attribute out = new Dwarf_Attribute_s();
			out->a = a; out->dbg = die->dbg;
			*ret_attr = out;
			return DW_DLV_OK;
		}

		namespace {
			struct attrlist_ctx {
				Dwarf_Debug_s *dbg;
				std::vector<Dwarf_Attribute_s*> *out;
			};
			int attrlist_cb(::Dwarf_Attribute *a, void *arg)
			{
				attrlist_ctx *ctx = static_cast<attrlist_ctx*>(arg);
				Dwarf_Attribute_s *w = new Dwarf_Attribute_s();
				w->a = *a; w->dbg = ctx->dbg;
				ctx->out->push_back(w);
				return DWARF_CB_OK;
			}
		}

		int dwarf_attrlist(Dwarf_Die die, Dwarf_Attribute **attrbuf,
			Dwarf_Signed *attrcount, Dwarf_Error * /*error*/)
		{
			std::vector<Dwarf_Attribute_s*> collected;
			attrlist_ctx ctx { die->dbg, &collected };
			/* Our callback always returns DWARF_CB_OK, so a single pass visits
			 * every attribute; libdw then returns 1 ("done") or -1 (error). */
			ptrdiff_t off = ::dwarf_getattrs(&die->d, attrlist_cb, &ctx, 0);
			if (off < 0)
			{
				for (auto p : collected) delete p;
				return DW_DLV_ERROR;
			}
			if (collected.empty()) return DW_DLV_NO_ENTRY;
			Dwarf_Attribute *arr = static_cast<Dwarf_Attribute*>(
				malloc(collected.size() * sizeof(Dwarf_Attribute)));
			for (size_t i = 0; i < collected.size(); ++i) arr[i] = collected[i];
			*attrbuf = arr;
			*attrcount = (Dwarf_Signed) collected.size();
			return DW_DLV_OK;
		}

		int dwarf_whatattr(Dwarf_Attribute attr, Dwarf_Half *ret_attr,
			Dwarf_Error * /*error*/)
		{
			*ret_attr = (Dwarf_Half) ::dwarf_whatattr(&attr->a);
			return DW_DLV_OK;
		}

		int dwarf_whatform(Dwarf_Attribute attr, Dwarf_Half *ret_form,
			Dwarf_Error * /*error*/)
		{
			*ret_form = (Dwarf_Half) ::dwarf_whatform(&attr->a);
			return DW_DLV_OK;
		}

		/* ---- form value extraction -------------------------------------- */
		int dwarf_formstring(Dwarf_Attribute attr, char **ret, Dwarf_Error * /*error*/)
		{
			const char *s = ::dwarf_formstring(&attr->a);
			if (!s) return DW_DLV_ERROR;
			*ret = const_cast<char*>(s);
			return DW_DLV_OK;
		}

		int dwarf_formflag(Dwarf_Attribute attr, Dwarf_Bool *ret, Dwarf_Error * /*error*/)
		{
			bool b = false;
			if (::dwarf_formflag(&attr->a, &b) != 0) return DW_DLV_ERROR;
			*ret = b ? 1 : 0;
			return DW_DLV_OK;
		}

		int dwarf_formaddr(Dwarf_Attribute attr, Dwarf_Addr *ret, Dwarf_Error * /*error*/)
		{
			::Dwarf_Addr a = 0;
			if (::dwarf_formaddr(&attr->a, &a) != 0) return DW_DLV_ERROR;
			*ret = a;
			return DW_DLV_OK;
		}

		int dwarf_formudata(Dwarf_Attribute attr, Dwarf_Unsigned *ret, Dwarf_Error * /*error*/)
		{
			::Dwarf_Word w = 0;
			if (::dwarf_formudata(&attr->a, &w) != 0) return DW_DLV_ERROR;
			*ret = w;
			return DW_DLV_OK;
		}

		int dwarf_formsdata(Dwarf_Attribute attr, Dwarf_Signed *ret, Dwarf_Error * /*error*/)
		{
			::Dwarf_Sword w = 0;
			if (::dwarf_formsdata(&attr->a, &w) != 0) return DW_DLV_ERROR;
			*ret = w;
			return DW_DLV_OK;
		}

		int dwarf_global_formref(Dwarf_Attribute attr, Dwarf_Off *ret, Dwarf_Error * /*error*/)
		{
			/* Reference forms (DW_FORM_ref*) resolve to a DIE; section-offset
			 * forms (DW_FORM_sec_offset, data4/8) are read as a raw value. */
			::Dwarf_Die d;
			if (::dwarf_formref_die(&attr->a, &d) != nullptr)
			{
				*ret = ::dwarf_dieoffset(&d);
				return DW_DLV_OK;
			}
			::Dwarf_Word w = 0;
			if (::dwarf_formudata(&attr->a, &w) == 0)
			{
				*ret = w;
				return DW_DLV_OK;
			}
			return DW_DLV_ERROR;
		}

		int dwarf_formblock(Dwarf_Attribute attr, Dwarf_Block **ret, Dwarf_Error * /*error*/)
		{
			::Dwarf_Block blk;
			if (::dwarf_formblock(&attr->a, &blk) != 0) return DW_DLV_ERROR;
			Dwarf_Block *out = new Dwarf_Block();
			out->bl_len = blk.length;
			out->bl_data = blk.data;
			out->bl_from_loclist = 0;
			out->bl_section_offset = 0;
			*ret = out;
			return DW_DLV_OK;
		}

		/* ---- deallocation / errors -------------------------------------- *
		 * libdw owns the storage behind names, strings and block data, so we
		 * only ever free the little wrappers (and arrays) that we allocated. */
		void dwarf_dealloc(Dwarf_Debug /*dbg*/, void *space, Dwarf_Unsigned type)
		{
			if (!space || space == (void*)-1) return;
			switch (type)
			{
				case DW_DLA_DIE:
					delete static_cast<Dwarf_Die_s*>(space);
					break;
				case DW_DLA_ATTR:
					delete static_cast<Dwarf_Attribute_s*>(space);
					break;
				case DW_DLA_BLOCK:
					delete static_cast<Dwarf_Block*>(space);
					break;
				case DW_DLA_LOCDESC:
				case DW_DLA_LOC_BLOCK:
				case DW_DLA_LIST:
					/* Location descriptors, their Dwarf_Loc arrays, and our
					 * pointer-arrays are all malloc'd by the loclist code below. */
					free(space);
					break;
				case DW_DLA_STRING:
				case DW_DLA_ERROR:
				default:
					/* nothing we own */
					break;
			}
		}

		const char *dwarf_errmsg(Dwarf_Error /*error*/)
		{
			const char *m = ::dwarf_errmsg(-1);
			return m ? m : "(no libdw error)";
		}

		int dwarf_errno(Dwarf_Error /*error*/)
		{
			return ::dwarf_errno();
		}

		/* ================================================================== *
		 *  Location lists and range lists, over libdw.                         *
		 * ================================================================== */
		namespace {
			/* Build a malloc'd Dwarf_Loc[] from libdw's parsed Dwarf_Op[]. The
			 * two structs are field-for-field equivalent. */
			Dwarf_Loc *loc_array_from_ops(const ::Dwarf_Op *ops, size_t n)
			{
				Dwarf_Loc *arr = static_cast<Dwarf_Loc*>(malloc((n ? n : 1) * sizeof(Dwarf_Loc)));
				for (size_t i = 0; i < n; ++i)
				{
					arr[i].lr_atom    = ops[i].atom;
					arr[i].lr_number  = ops[i].number;
					arr[i].lr_number2 = ops[i].number2;
					arr[i].lr_offset  = ops[i].offset;
				}
				return arr;
			}
			Dwarf_Locdesc *make_locdesc(Dwarf_Loc *locs, Dwarf_Half cents,
				Dwarf_Addr lopc, Dwarf_Addr hipc)
			{
				Dwarf_Locdesc *ld = static_cast<Dwarf_Locdesc*>(malloc(sizeof(Dwarf_Locdesc)));
				ld->ld_lopc = lopc;
				ld->ld_hipc = hipc;
				ld->ld_cents = cents;
				ld->ld_s = locs;
				ld->ld_from_loclist = 0;
				ld->ld_section_offset = 0;
				return ld;
			}

			/* Minimal LEB128 readers over a byte buffer. */
			uint64_t read_uleb(const unsigned char *p, size_t len, size_t &i)
			{
				uint64_t r = 0; unsigned shift = 0;
				while (i < len)
				{
					unsigned char b = p[i++];
					r |= uint64_t(b & 0x7f) << shift;
					if (!(b & 0x80)) break;
					shift += 7;
				}
				return r;
			}
			int64_t read_sleb(const unsigned char *p, size_t len, size_t &i)
			{
				int64_t r = 0; unsigned shift = 0; unsigned char b = 0;
				while (i < len)
				{
					b = p[i++];
					r |= int64_t(b & 0x7f) << shift;
					shift += 7;
					if (!(b & 0x80)) break;
				}
				if (shift < 64 && (b & 0x40)) r |= -(int64_t(1) << shift);
				return r;
			}
			template <typename T> uint64_t read_fixed(const unsigned char *p, size_t len, size_t &i)
			{
				T v = 0;
				if (i + sizeof(T) <= len) { memcpy(&v, p + i, sizeof(T)); i += sizeof(T); }
				return (uint64_t)v;
			}

			/* Decode a DWARF location/CFA expression's bytes into Dwarf_Locs.
			 * Returns false if an unrecognised opcode is hit (whose operand
			 * length we cannot know), so the caller can degrade gracefully --
			 * mirroring libdwarf's "didn't understand the expression" behaviour.
			 * addr_size/offset_size default to 8/4 (we are not told them here). */
			bool parse_expr(const unsigned char *p, size_t len,
				unsigned addr_size, unsigned offset_size, std::vector<Dwarf_Loc> &out)
			{
				size_t i = 0;
				while (i < len)
				{
					Dwarf_Loc loc; memset(&loc, 0, sizeof loc);
					loc.lr_offset = i;
					unsigned char op = p[i++];
					loc.lr_atom = op;
					if (op >= DW_OP_lit0 && op <= DW_OP_lit31) { /* no operand */ }
					else if (op >= DW_OP_reg0 && op <= DW_OP_reg31) { /* no operand */ }
					else if (op >= DW_OP_breg0 && op <= DW_OP_breg31)
						loc.lr_number = (Dwarf_Unsigned) read_sleb(p, len, i);
					else switch (op)
					{
						/* operandless */
						case DW_OP_deref: case DW_OP_dup: case DW_OP_drop:
						case DW_OP_over: case DW_OP_swap: case DW_OP_rot:
						case DW_OP_xderef: case DW_OP_abs: case DW_OP_and:
						case DW_OP_div: case DW_OP_minus: case DW_OP_mod:
						case DW_OP_mul: case DW_OP_neg: case DW_OP_not:
						case DW_OP_or: case DW_OP_plus: case DW_OP_shl:
						case DW_OP_shr: case DW_OP_shra: case DW_OP_xor:
						case DW_OP_eq: case DW_OP_ge: case DW_OP_gt:
						case DW_OP_le: case DW_OP_lt: case DW_OP_ne:
						case DW_OP_nop: case DW_OP_push_object_address:
						case DW_OP_form_tls_address: case DW_OP_call_frame_cfa:
						case DW_OP_stack_value:
							break;
						/* single fixed-size unsigned operand */
						case DW_OP_const1u: case DW_OP_pick:
						case DW_OP_deref_size: case DW_OP_xderef_size:
							loc.lr_number = read_fixed<uint8_t>(p, len, i); break;
						case DW_OP_const1s:
							loc.lr_number = (Dwarf_Unsigned)(int64_t)(int8_t)read_fixed<uint8_t>(p, len, i); break;
						case DW_OP_const2u: case DW_OP_call2:
							loc.lr_number = read_fixed<uint16_t>(p, len, i); break;
						case DW_OP_const2s: case DW_OP_skip: case DW_OP_bra:
							loc.lr_number = (Dwarf_Unsigned)(int64_t)(int16_t)read_fixed<uint16_t>(p, len, i); break;
						case DW_OP_const4u: case DW_OP_call4:
							loc.lr_number = read_fixed<uint32_t>(p, len, i); break;
						case DW_OP_const4s:
							loc.lr_number = (Dwarf_Unsigned)(int64_t)(int32_t)read_fixed<uint32_t>(p, len, i); break;
						case DW_OP_const8u: case DW_OP_const8s:
							loc.lr_number = read_fixed<uint64_t>(p, len, i); break;
						/* address / section-offset sized operand */
						case DW_OP_addr:
							loc.lr_number = (addr_size == 4)
								? read_fixed<uint32_t>(p, len, i)
								: read_fixed<uint64_t>(p, len, i);
							break;
						case DW_OP_call_ref:
							loc.lr_number = (offset_size == 8)
								? read_fixed<uint64_t>(p, len, i)
								: read_fixed<uint32_t>(p, len, i);
							break;
						/* single ULEB operand */
						case DW_OP_constu: case DW_OP_plus_uconst: case DW_OP_regx:
						case DW_OP_piece: case DW_OP_addrx: case DW_OP_constx:
							loc.lr_number = (Dwarf_Unsigned) read_uleb(p, len, i); break;
						/* single SLEB operand */
						case DW_OP_consts: case DW_OP_fbreg:
							loc.lr_number = (Dwarf_Unsigned) read_sleb(p, len, i); break;
						/* ULEB register + SLEB offset */
						case DW_OP_bregx:
							loc.lr_number  = (Dwarf_Unsigned) read_uleb(p, len, i);
							loc.lr_number2 = (Dwarf_Unsigned) read_sleb(p, len, i);
							break;
						/* two ULEB operands */
						case DW_OP_bit_piece:
							loc.lr_number  = (Dwarf_Unsigned) read_uleb(p, len, i);
							loc.lr_number2 = (Dwarf_Unsigned) read_uleb(p, len, i);
							break;
						/* ULEB length + that many bytes of sub-block; we record the
						 * length and skip the bytes. */
						case DW_OP_implicit_value:
						case DW_OP_entry_value:
						{
							uint64_t l = read_uleb(p, len, i);
							loc.lr_number = (Dwarf_Unsigned) l;
							i += l;
							break;
						}
						default:
							/* unknown opcode: cannot know operand size */
							return false;
					}
					if (i > len) return false; /* operand ran past the buffer */
					out.push_back(loc);
				}
				return true;
			}
		} // anonymous namespace

		int dwarf_loclist_n(Dwarf_Attribute attr, Dwarf_Locdesc ***llbuf,
			Dwarf_Signed *loc_count, Dwarf_Error * /*error*/)
		{
			/* dwarf_getlocations enumerates each entry of a location list (or the
			 * single expression of an exprloc/block) as a parsed Dwarf_Op array
			 * plus its [startp,endp) PC range. */
			std::vector<Dwarf_Locdesc*> descs;
			ptrdiff_t off = 0;
			::Dwarf_Addr base = 0, start = 0, end = 0;
			::Dwarf_Op *ops = nullptr; size_t nops = 0;
			while ((off = ::dwarf_getlocations(&attr->a, off, &base, &start, &end,
				&ops, &nops)) > 0)
			{
				Dwarf_Loc *locs = loc_array_from_ops(ops, nops);
				descs.push_back(make_locdesc(locs, (Dwarf_Half) nops, start, end));
			}
			if (off < 0)
			{
				for (auto *d : descs) { free(d->ld_s); free(d); }
				return DW_DLV_ERROR;
			}
			if (descs.empty()) return DW_DLV_NO_ENTRY;
			Dwarf_Locdesc **arr = static_cast<Dwarf_Locdesc**>(
				malloc(descs.size() * sizeof(Dwarf_Locdesc*)));
			for (size_t i = 0; i < descs.size(); ++i) arr[i] = descs[i];
			*llbuf = arr;
			*loc_count = (Dwarf_Signed) descs.size();
			return DW_DLV_OK;
		}

		int dwarf_loclist_from_expr(Dwarf_Debug /*dbg*/, Dwarf_Ptr bytes_in,
			Dwarf_Unsigned bytes_len, Dwarf_Locdesc **llbuf,
			Dwarf_Signed *list_len, Dwarf_Error * /*error*/)
		{
			std::vector<Dwarf_Loc> parsed;
			if (!parse_expr(static_cast<const unsigned char*>(bytes_in),
				(size_t) bytes_len, /*addr_size*/8, /*offset_size*/4, parsed))
			{
				return DW_DLV_ERROR; /* unrecognised opcode -> caller degrades */
			}
			Dwarf_Loc *locs = static_cast<Dwarf_Loc*>(
				malloc((parsed.size() ? parsed.size() : 1) * sizeof(Dwarf_Loc)));
			for (size_t i = 0; i < parsed.size(); ++i) locs[i] = parsed[i];
			*llbuf = make_locdesc(locs, (Dwarf_Half) parsed.size(), 0, 0);
			*list_len = 1;
			return DW_DLV_OK;
		}

		int dwarf_formexprloc(Dwarf_Attribute attr, Dwarf_Unsigned *ret_exprlen,
			Dwarf_Ptr *block_ptr, Dwarf_Error * /*error*/)
		{
			/* DW_FORM_exprloc is a counted block; libdw's dwarf_formblock yields
			 * its raw bytes, which the handle layer then hands to
			 * dwarf_loclist_from_expr above. */
			::Dwarf_Block blk;
			if (::dwarf_formblock(&attr->a, &blk) != 0) return DW_DLV_NO_ENTRY;
			if (ret_exprlen) *ret_exprlen = blk.length;
			if (block_ptr) *block_ptr = blk.data;
			return DW_DLV_OK;
		}

		/* dwarf_get_ranges{,_a} read a .debug_ranges/.debug_rnglists list. The
		 * libdwarf API keys on a section offset; libdw's dwarf_ranges keys on the
		 * owning DIE, so the _a form (which has the DIE) is the one we can serve.
		 * libdw resolves base-address selection internally, so every entry we
		 * emit is a plain DW_RANGES_ENTRY, terminated by a DW_RANGES_END. */
		static int get_ranges_via_die(Dwarf_Die die, Dwarf_Ranges **rangesbuf,
			Dwarf_Signed *listlen, Dwarf_Unsigned *bytecount)
		{
			if (!die) return DW_DLV_NO_ENTRY;
			std::vector<Dwarf_Ranges> v;
			ptrdiff_t off = 0;
			::Dwarf_Addr base = 0, start = 0, end = 0;
			while ((off = ::dwarf_ranges(&die->d, off, &base, &start, &end)) > 0)
			{
				Dwarf_Ranges r;
				r.dwr_addr1 = start; r.dwr_addr2 = end; r.dwr_type = DW_RANGES_ENTRY;
				v.push_back(r);
			}
			if (off < 0) return DW_DLV_ERROR;
			if (v.empty()) return DW_DLV_NO_ENTRY;
			Dwarf_Ranges term; term.dwr_addr1 = 0; term.dwr_addr2 = 0;
			term.dwr_type = DW_RANGES_END;
			v.push_back(term);
			Dwarf_Ranges *arr = static_cast<Dwarf_Ranges*>(
				malloc(v.size() * sizeof(Dwarf_Ranges)));
			for (size_t i = 0; i < v.size(); ++i) arr[i] = v[i];
			*rangesbuf = arr;
			*listlen = (Dwarf_Signed) v.size();
			if (bytecount) *bytecount = 0;
			return DW_DLV_OK;
		}
		int dwarf_get_ranges(Dwarf_Debug, Dwarf_Off, Dwarf_Ranges **,
			Dwarf_Signed *, Dwarf_Unsigned *, Dwarf_Error *)
		{
			/* No owning DIE available here; the DIE-aware _a form is used by
			 * libdwarfpp's range handling, so this remains unsupported. */
			return DW_DLV_NO_ENTRY;
		}
		int dwarf_get_ranges_a(Dwarf_Debug, Dwarf_Off, Dwarf_Die die,
			Dwarf_Ranges **rangesbuf, Dwarf_Signed *listlen,
			Dwarf_Unsigned *bytecount, Dwarf_Error * /*error*/)
		{
			return get_ranges_via_die(die, rangesbuf, listlen, bytecount);
		}
		void dwarf_ranges_dealloc(Dwarf_Debug, Dwarf_Ranges *rangesbuf, Dwarf_Signed)
		{
			if (rangesbuf && rangesbuf != (Dwarf_Ranges*)-1) free(rangesbuf);
		}
		int dwarf_srcfiles(Dwarf_Die die, char ***srcfiles, Dwarf_Signed *count,
			Dwarf_Error * /*error*/)
		{
			/* libdwarf's dwarf_srcfiles returns a 0-based array whose element i
			 * is DWARF file number i+1 (file 0 means "no file"); libdwarfpp
			 * indexes it as names[decl_file - 1]. libdw's dwarf_filesrc takes the
			 * DWARF file number directly (index 0 being the reserved slot), so we
			 * map element i -> dwarf_filesrc(files, i + 1). */
			::Dwarf_Files *files = nullptr; size_t n = 0;
			if (::dwarf_getsrcfiles(&die->d, &files, &n) != 0) return DW_DLV_ERROR;
			if (n <= 1) return DW_DLV_NO_ENTRY;
			size_t m = n - 1;
			char **arr = static_cast<char**>(malloc(m * sizeof(char*)));
			for (size_t i = 0; i < m; ++i)
			{
				const char *s = ::dwarf_filesrc(files, i + 1, nullptr, nullptr);
				arr[i] = const_cast<char*>(s ? s : "");
			}
			*srcfiles = arr;
			*count = (Dwarf_Signed) m;
			return DW_DLV_OK;
		}

		/* ================================================================== *
		 *  Frame / CFI, over libdw's dwarf_next_cfi (.debug_frame/.eh_frame). *
		 *                                                                     *
		 *  libdw's higher-level CFI API (dwarf_cfi_addrframe) only yields the *
		 *  *computed* unwind state at an address, whereas libdwarfpp's        *
		 *  FrameSection iterates raw CIEs/FDEs and interprets the instruction *
		 *  streams itself. So we parse the frame section with dwarf_next_cfi  *
		 *  -- which frames each entry and points into the section data -- and  *
		 *  present the result as libdwarf's FDE/CIE handle API. The Cie/Fde    *
		 *  handles are pointers into structures we own; equality is pointer    *
		 *  identity, as libdwarfpp expects.                                    *
		 * ================================================================== */

		namespace {
			uint64_t read_uN(const uint8_t *&p, const uint8_t *end, unsigned n)
			{
				uint64_t v = 0;
				for (unsigned i = 0; i < n && p < end; ++i) v |= uint64_t(*p++) << (8 * i);
				return v;
			}
			uint64_t uleb_p(const uint8_t *&p, const uint8_t *end)
			{
				uint64_t r = 0; unsigned s = 0;
				while (p < end) { uint8_t b = *p++; r |= uint64_t(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; }
				return r;
			}
			int64_t sleb_p(const uint8_t *&p, const uint8_t *end)
			{
				int64_t r = 0; unsigned s = 0; uint8_t b = 0;
				while (p < end) { b = *p++; r |= int64_t(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
				if (s < 64 && (b & 0x40)) r |= -(int64_t(1) << s);
				return r;
			}
			int64_t sign_extend(uint64_t v, unsigned bytes)
			{
				unsigned bits = bytes * 8;
				if (bits < 64 && (v & (uint64_t(1) << (bits - 1)))) v |= ~((uint64_t(1) << bits) - 1);
				return (int64_t) v;
			}
			/* Read a DW_EH_PE-encoded value (the format part only), advancing p. */
			uint64_t read_eh_value(unsigned char enc, const uint8_t *&p, const uint8_t *end,
				unsigned addr_size)
			{
				switch (enc & 0x0f)
				{
					case DW_EH_PE_absptr:  return read_uN(p, end, addr_size);
					case DW_EH_PE_uleb128: return uleb_p(p, end);
					case DW_EH_PE_sleb128: return (uint64_t) sleb_p(p, end);
					case DW_EH_PE_udata2:  return read_uN(p, end, 2);
					case DW_EH_PE_udata4:  return read_uN(p, end, 4);
					case DW_EH_PE_udata8:  return read_uN(p, end, 8);
					case DW_EH_PE_sdata2:  return (uint64_t) sign_extend(read_uN(p, end, 2), 2);
					case DW_EH_PE_sdata4:  return (uint64_t) sign_extend(read_uN(p, end, 4), 4);
					case DW_EH_PE_sdata8:  return read_uN(p, end, 8);
					default:               return 0;
				}
			}
			/* Read a DW_EH_PE-encoded pointer, applying the base for pcrel.
			 * value_vaddr is the run-time address of the encoded value itself. */
			uint64_t read_eh_pointer(unsigned char enc, const uint8_t *&p, const uint8_t *end,
				uint64_t value_vaddr, unsigned addr_size)
			{
				if (enc == 0xff /* DW_EH_PE_omit */) return 0;
				uint64_t raw = read_eh_value(enc, p, end, addr_size);
				switch (enc & 0x70)
				{
					case DW_EH_PE_pcrel: return value_vaddr + raw;
					default:             return raw; /* absptr (and best-effort others) */
				}
			}
			/* Determine the FDE pointer encoding ('R') from a CIE augmentation. */
			unsigned char cie_fde_encoding(const char *aug, const uint8_t *augdata,
				size_t augsz, unsigned addr_size)
			{
				if (!aug || aug[0] != 'z') return DW_EH_PE_absptr;
				const uint8_t *p = augdata, *end = augdata + augsz;
				for (const char *c = aug + 1; *c; ++c)
				{
					switch (*c)
					{
						case 'R': return (p < end) ? *p : (unsigned char) DW_EH_PE_absptr;
						case 'L': if (p < end) ++p; break;
						case 'S': break;
						case 'P': {
							if (p >= end) return DW_EH_PE_absptr;
							unsigned char penc = *p++;
							read_eh_value(penc, p, end, addr_size); /* skip personality ptr */
						} break;
						default: return DW_EH_PE_absptr;
					}
				}
				return DW_EH_PE_absptr;
			}
		} // anonymous namespace

		/* Full definitions of the opaque CFI handle types. */
		struct Dwarf_Cie_s {
			Dwarf_Off offset;
			Dwarf_Unsigned bytes_in_cie;
			Dwarf_Small version;
			char *augmenter;                  /* points into section data */
			Dwarf_Unsigned code_alignment_factor;
			Dwarf_Signed data_alignment_factor;
			Dwarf_Half return_address_register;
			Dwarf_Ptr initial_instructions;   /* points into section data */
			Dwarf_Unsigned initial_instructions_length;
			Dwarf_Signed index;
			unsigned char fde_encoding;
			size_t fde_aug_data_size;
			bool has_z;
		};
		struct Dwarf_Fde_s {
			Dwarf_Off offset;
			Dwarf_Addr low_pc;
			Dwarf_Unsigned func_length;
			Dwarf_Ptr fde_bytes;              /* points into section data */
			Dwarf_Unsigned fde_byte_length;
			Dwarf_Ptr instr_bytes;            /* points into section data */
			Dwarf_Unsigned instr_len;
			Dwarf_Cie cie;
			Dwarf_Signed cie_index;
			Dwarf_Off cie_offset_field;       /* value for dwarf_get_fde_range */
		};

		static int build_fde_list(Dwarf_Debug dbg, bool eh,
			Dwarf_Cie **cie_data, Dwarf_Signed *cie_count,
			Dwarf_Fde **fde_data, Dwarf_Signed *fde_count)
		{
			::Elf *elf = ::dwarf_getelf(dbg->dw);
			if (!elf) return DW_DLV_NO_ENTRY;
			size_t shstrndx = 0;
			if (::elf_getshdrstrndx(elf, &shstrndx) != 0) return DW_DLV_NO_ENTRY;
			const char *want = eh ? ".eh_frame" : ".debug_frame";
			Elf_Scn *scn = nullptr; Elf_Data *data = nullptr;
			::GElf_Addr sh_addr = 0; bool found = false;
			for (Elf_Scn *s = nullptr; (s = ::elf_nextscn(elf, s)); )
			{
				::GElf_Shdr sh;
				if (!::gelf_getshdr(s, &sh)) continue;
				const char *nm = ::elf_strptr(elf, shstrndx, sh.sh_name);
				if (nm && strcmp(nm, want) == 0) { scn = s; sh_addr = sh.sh_addr; found = true; break; }
			}
			if (!found) return DW_DLV_NO_ENTRY;
			data = ::elf_getdata(scn, nullptr);
			if (!data || !data->d_buf) return DW_DLV_NO_ENTRY;
			const uint8_t *sec = static_cast<const uint8_t*>(data->d_buf);
			unsigned char *eident = reinterpret_cast<unsigned char*>(::elf_getident(elf, nullptr));
			unsigned addr_size = (eident && eident[EI_CLASS] == ELFCLASS64) ? 8 : 4;

			std::vector<Dwarf_Cie_s*> cies;
			std::vector<Dwarf_Fde_s*> fdes;
			std::map< ::Dwarf_Off, Dwarf_Cie_s*> cie_by_off;

			::Dwarf_Off off = 0, next = 0;
			::Dwarf_CFI_Entry entry;
			int r;
			while ((r = ::dwarf_next_cfi(eident, data, eh, off, &next, &entry)) == 0)
			{
				if (dwarf_cfi_cie_p(&entry))
				{
					Dwarf_Cie_s *c = new Dwarf_Cie_s();
					c->offset = off;
					c->augmenter = const_cast<char*>(entry.cie.augmentation ? entry.cie.augmentation : "");
					c->version = entry.cie.augmentation
						? *(reinterpret_cast<const uint8_t*>(entry.cie.augmentation) - 1) : 1;
					c->code_alignment_factor = entry.cie.code_alignment_factor;
					c->data_alignment_factor = entry.cie.data_alignment_factor;
					c->return_address_register = (Dwarf_Half) entry.cie.return_address_register;
					c->initial_instructions = const_cast<uint8_t*>(entry.cie.initial_instructions);
					c->initial_instructions_length =
						entry.cie.initial_instructions_end - entry.cie.initial_instructions;
					bool is64 = (sec + off + 4 <= sec + data->d_size)
						&& (*reinterpret_cast<const uint32_t*>(sec + off) == 0xffffffffu);
					c->bytes_in_cie = (Dwarf_Unsigned)(next - off) - (is64 ? 12 : 4);
					c->index = (Dwarf_Signed) cies.size();
					c->fde_encoding = eh
						? cie_fde_encoding(c->augmenter, entry.cie.augmentation_data,
							entry.cie.augmentation_data_size, addr_size)
						: (unsigned char) DW_EH_PE_absptr;
					c->fde_aug_data_size = entry.cie.fde_augmentation_data_size;
					c->has_z = (c->augmenter[0] == 'z');
					cies.push_back(c);
					cie_by_off[off] = c;
				}
				else
				{
					Dwarf_Fde_s *f = new Dwarf_Fde_s();
					f->offset = off;
					::Dwarf_Off abs_cie_off = entry.fde.CIE_pointer;
					auto found_cie = cie_by_off.find(abs_cie_off);
					Dwarf_Cie_s *c = (found_cie != cie_by_off.end()) ? found_cie->second : nullptr;
					f->cie = c;
					f->cie_index = c ? c->index : 0;
					const uint8_t *p = entry.fde.start;
					const uint8_t *pend = entry.fde.end;
					unsigned char enc = c ? c->fde_encoding : (unsigned char) DW_EH_PE_absptr;
					uint64_t loc_vaddr = sh_addr + (uint64_t)(p - sec);
					f->low_pc = read_eh_pointer(enc, p, pend, loc_vaddr, addr_size);
					/* address_range is a length: same format, no base applied. */
					f->func_length = read_eh_value(enc, p, pend, addr_size);
					/* skip the FDE's augmentation data to reach the instructions */
					if (c && c->has_z) { uint64_t al = uleb_p(p, pend); p += al; }
					else if (c) p += c->fde_aug_data_size;
					if (p > pend) p = pend;
					f->instr_bytes = const_cast<uint8_t*>(p);
					f->instr_len = (Dwarf_Unsigned)(pend - p);
					f->fde_bytes = const_cast<uint8_t*>(sec + off);
					/* fde_byte_length is the value of the FDE's length field, i.e.
					 * excludes the length field itself (matching libdwarf/readelf). */
					bool fde_is64 = (sec + off + 4 <= sec + data->d_size)
						&& (*reinterpret_cast<const uint32_t*>(sec + off) == 0xffffffffu);
					f->fde_byte_length = (Dwarf_Unsigned)(next - off) - (fde_is64 ? 12 : 4);
					/* libdwarf reports, for .eh_frame, the *relative* CIE pointer that
					 * FrameSection turns back into an absolute offset; for .debug_frame
					 * it is already absolute. (FrameSection assumes 32-bit DWARF format.) */
					f->cie_offset_field = eh
						? (Dwarf_Off)(off + 4) - abs_cie_off
						: abs_cie_off;
					fdes.push_back(f);
				}
				off = next;
			}

			*cie_count = (Dwarf_Signed) cies.size();
			*fde_count = (Dwarf_Signed) fdes.size();
			Dwarf_Cie *ca = static_cast<Dwarf_Cie*>(malloc((cies.size() + 1) * sizeof(Dwarf_Cie)));
			for (size_t i = 0; i < cies.size(); ++i) ca[i] = cies[i];
			ca[cies.size()] = nullptr;
			Dwarf_Fde *fa = static_cast<Dwarf_Fde*>(malloc((fdes.size() + 1) * sizeof(Dwarf_Fde)));
			for (size_t i = 0; i < fdes.size(); ++i) fa[i] = fdes[i];
			fa[fdes.size()] = nullptr;
			*cie_data = ca; *fde_data = fa;
			if (cies.empty() && fdes.empty()) return DW_DLV_NO_ENTRY;
			return DW_DLV_OK;
		}

		int dwarf_get_fde_list(Dwarf_Debug dbg, Dwarf_Cie **cie_data,
			Dwarf_Signed *cie_count, Dwarf_Fde **fde_data,
			Dwarf_Signed *fde_count, Dwarf_Error * /*error*/)
		{ return build_fde_list(dbg, false, cie_data, cie_count, fde_data, fde_count); }

		int dwarf_get_fde_list_eh(Dwarf_Debug dbg, Dwarf_Cie **cie_data,
			Dwarf_Signed *cie_count, Dwarf_Fde **fde_data,
			Dwarf_Signed *fde_count, Dwarf_Error * /*error*/)
		{ return build_fde_list(dbg, true, cie_data, cie_count, fde_data, fde_count); }

		void dwarf_fde_cie_list_dealloc(Dwarf_Debug, Dwarf_Cie *cie_data,
			Dwarf_Signed cie_count, Dwarf_Fde *fde_data, Dwarf_Signed fde_count)
		{
			if (cie_data) { for (Dwarf_Signed i = 0; i < cie_count; ++i) delete cie_data[i]; free(cie_data); }
			if (fde_data) { for (Dwarf_Signed i = 0; i < fde_count; ++i) delete fde_data[i]; free(fde_data); }
		}

		int dwarf_get_fde_range(Dwarf_Fde fde, Dwarf_Addr *low_pc,
			Dwarf_Unsigned *func_length, Dwarf_Ptr *fde_bytes,
			Dwarf_Unsigned *fde_byte_length, Dwarf_Off *cie_offset,
			Dwarf_Signed *cie_index, Dwarf_Off *fde_offset, Dwarf_Error * /*error*/)
		{
			if (!fde) return DW_DLV_NO_ENTRY;
			if (low_pc)          *low_pc = fde->low_pc;
			if (func_length)     *func_length = fde->func_length;
			if (fde_bytes)       *fde_bytes = fde->fde_bytes;
			if (fde_byte_length) *fde_byte_length = fde->fde_byte_length;
			if (cie_offset)      *cie_offset = fde->cie_offset_field;
			if (cie_index)       *cie_index = fde->cie_index;
			if (fde_offset)      *fde_offset = fde->offset;
			return DW_DLV_OK;
		}

		int dwarf_get_cie_of_fde(Dwarf_Fde fde, Dwarf_Cie *cie_returned, Dwarf_Error * /*error*/)
		{
			if (!fde || !fde->cie) return DW_DLV_NO_ENTRY;
			*cie_returned = fde->cie;
			return DW_DLV_OK;
		}

		int dwarf_get_cie_info(Dwarf_Cie cie, Dwarf_Unsigned *bytes_in_cie,
			Dwarf_Small *version, char **augmenter,
			Dwarf_Unsigned *code_alignment_factor, Dwarf_Signed *data_alignment_factor,
			Dwarf_Half *return_address_register_rule, Dwarf_Ptr *initial_instructions,
			Dwarf_Unsigned *initial_instructions_length, Dwarf_Error * /*error*/)
		{
			if (!cie) return DW_DLV_NO_ENTRY;
			if (bytes_in_cie)                 *bytes_in_cie = cie->bytes_in_cie;
			if (version)                      *version = cie->version;
			if (augmenter)                    *augmenter = cie->augmenter;
			if (code_alignment_factor)        *code_alignment_factor = cie->code_alignment_factor;
			if (data_alignment_factor)        *data_alignment_factor = cie->data_alignment_factor;
			if (return_address_register_rule) *return_address_register_rule = cie->return_address_register;
			if (initial_instructions)         *initial_instructions = cie->initial_instructions;
			if (initial_instructions_length)  *initial_instructions_length = cie->initial_instructions_length;
			return DW_DLV_OK;
		}

		int dwarf_get_cie_index(Dwarf_Cie cie, Dwarf_Signed *index, Dwarf_Error * /*error*/)
		{
			if (!cie) return DW_DLV_NO_ENTRY;
			*index = cie->index;
			return DW_DLV_OK;
		}

		int dwarf_get_fde_instr_bytes(Dwarf_Fde fde, Dwarf_Ptr *outinstrs,
			Dwarf_Unsigned *outlen, Dwarf_Error * /*error*/)
		{
			if (!fde) return DW_DLV_NO_ENTRY;
			*outinstrs = fde->instr_bytes;
			*outlen = fde->instr_len;
			return DW_DLV_OK;
		}

		int dwarf_get_fde_at_pc(Dwarf_Fde *fde_data, Dwarf_Addr pc,
			Dwarf_Fde *returned_fde, Dwarf_Addr *lopc, Dwarf_Addr *hipc, Dwarf_Error * /*error*/)
		{
			/* fde_data is the NULL-terminated array we returned from build_fde_list. */
			if (!fde_data) return DW_DLV_NO_ENTRY;
			for (Dwarf_Fde *p = fde_data; *p; ++p)
			{
				Dwarf_Fde f = *p;
				if (pc >= f->low_pc && pc < f->low_pc + f->func_length)
				{
					if (returned_fde) *returned_fde = f;
					if (lopc) *lopc = f->low_pc;
					if (hipc) *hipc = f->low_pc + f->func_length;
					return DW_DLV_OK;
				}
			}
			return DW_DLV_NO_ENTRY;
		}

		int dwarf_get_CFA_name(unsigned int val_in, const char **s_out)
		{
			/* val_in is the reconstructed opcode byte (primary<<6 | extended). */
			switch (val_in & 0xc0)
			{
				case DW_CFA_advance_loc: *s_out = "DW_CFA_advance_loc"; return DW_DLV_OK;
				case DW_CFA_offset:      *s_out = "DW_CFA_offset";      return DW_DLV_OK;
				case DW_CFA_restore:     *s_out = "DW_CFA_restore";     return DW_DLV_OK;
				default: break;
			}
			switch (val_in)
			{
				case DW_CFA_nop:                *s_out = "DW_CFA_nop"; break;
				case DW_CFA_set_loc:            *s_out = "DW_CFA_set_loc"; break;
				case DW_CFA_advance_loc1:       *s_out = "DW_CFA_advance_loc1"; break;
				case DW_CFA_advance_loc2:       *s_out = "DW_CFA_advance_loc2"; break;
				case DW_CFA_advance_loc4:       *s_out = "DW_CFA_advance_loc4"; break;
				case DW_CFA_offset_extended:    *s_out = "DW_CFA_offset_extended"; break;
				case DW_CFA_restore_extended:   *s_out = "DW_CFA_restore_extended"; break;
				case DW_CFA_undefined:          *s_out = "DW_CFA_undefined"; break;
				case DW_CFA_same_value:         *s_out = "DW_CFA_same_value"; break;
				case DW_CFA_register:           *s_out = "DW_CFA_register"; break;
				case DW_CFA_remember_state:     *s_out = "DW_CFA_remember_state"; break;
				case DW_CFA_restore_state:      *s_out = "DW_CFA_restore_state"; break;
				case DW_CFA_def_cfa:            *s_out = "DW_CFA_def_cfa"; break;
				case DW_CFA_def_cfa_register:   *s_out = "DW_CFA_def_cfa_register"; break;
				case DW_CFA_def_cfa_offset:     *s_out = "DW_CFA_def_cfa_offset"; break;
				case DW_CFA_def_cfa_expression: *s_out = "DW_CFA_def_cfa_expression"; break;
				case DW_CFA_expression:         *s_out = "DW_CFA_expression"; break;
				case DW_CFA_offset_extended_sf: *s_out = "DW_CFA_offset_extended_sf"; break;
				case DW_CFA_def_cfa_sf:         *s_out = "DW_CFA_def_cfa_sf"; break;
				case DW_CFA_def_cfa_offset_sf:  *s_out = "DW_CFA_def_cfa_offset_sf"; break;
				case DW_CFA_val_offset:         *s_out = "DW_CFA_val_offset"; break;
				case DW_CFA_val_offset_sf:      *s_out = "DW_CFA_val_offset_sf"; break;
				case DW_CFA_val_expression:     *s_out = "DW_CFA_val_expression"; break;
				case DW_CFA_GNU_args_size:      *s_out = "DW_CFA_GNU_args_size"; break;
				case DW_CFA_GNU_window_save:    *s_out = "DW_CFA_GNU_window_save"; break;
				default:                        *s_out = "DW_CFA_unknown"; break;
			}
			return DW_DLV_OK;
		}

		/* dwarf_expand_frame_instructions is not needed: libdwarfpp decodes the
		 * instruction stream itself (encap::frame_instrlist). */
		int dwarf_expand_frame_instructions(Dwarf_Cie, Dwarf_Ptr, Dwarf_Unsigned,
			Dwarf_Frame_Op **, Dwarf_Signed *, Dwarf_Error *) { return DW_DLV_NO_ENTRY; }
		Dwarf_Half dwarf_set_frame_rule_table_size(Dwarf_Debug, Dwarf_Half value) { return value; }
		Dwarf_Half dwarf_set_frame_rule_initial_value(Dwarf_Debug, Dwarf_Half value) { return value; }
		Dwarf_Half dwarf_set_frame_cfa_value(Dwarf_Debug, Dwarf_Half value) { return value; }
		Dwarf_Half dwarf_set_frame_same_value(Dwarf_Debug, Dwarf_Half value) { return value; }
		Dwarf_Half dwarf_set_frame_undefined_value(Dwarf_Debug, Dwarf_Half value) { return value; }

	} // namespace lib
} // namespace dwarf
