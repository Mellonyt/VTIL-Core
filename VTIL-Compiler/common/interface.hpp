// Copyright (c) 2020 Can Boluk and contributors of the VTIL Project   
// All rights reserved.   
//    
// Redistribution and use in source and binary forms, with or without   
// modification, are permitted provided that the following conditions are met: 
//    
// 1. Redistributions of source code must retain the above copyright notice,   
//    this list of conditions and the following disclaimer.   
// 2. Redistributions in binary form must reproduce the above copyright   
//    notice, this list of conditions and the following disclaimer in the   
//    documentation and/or other materials provided with the distribution.   
// 3. Neither the name of VTIL Project nor the names of its contributors
//    may be used to endorse or promote products derived from this software 
//    without specific prior written permission.   
//    
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE   
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR   
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF   
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  
// POSSIBILITY OF SUCH DAMAGE.        
//
#pragma once
#include <algorithm>
#include <functional>
#include <atomic>
#include <vtil/utility>
#include <vtil/io>
#include <vtil/symex>
#include <vtil/arch>

namespace vtil::optimizer
{
	// Pass execution order.
	// - Note that while serial_<> asserts all links are processed is
	//   followed, parrellel_<> cannot do this, and that neither can
	//   assert whole path is processed.
	//
	enum class execution_order
	{
		custom,
		serial,
		serial_bf,
		serial_df,
		parallel,
		parallel_bf,
		parallel_df,
	};

	// Passes every block through the transformer given in parallel, returns the 
	// number of instances where this transformation was applied.
	//
	template<typename T>
	static auto apply_pass( routine* rtn, T* opt )
	{
		// Declare worker and allocate the final result.
		//
		std::atomic<size_t> n = { 0 };
		auto worker = [ & ] ( basic_block* block )
		{
			n += opt->pass( block, true );
		};

		// Switch based on order:
		//
		switch ( T::exec_order )
		{
			case execution_order::custom:
			{
				fassert( T::exec_order != execution_order::custom );
				break;
			}
			case execution_order::serial:
			{
				rtn->for_each( worker );
				break;
			}
			case execution_order::serial_bf:
			case execution_order::serial_df:
			{
				// Declare visit list and recursion helper.
				//
				path_set visited;
				visited.reserve( rtn->num_blocks() );
				auto rec = [ & ] ( basic_block* blk, auto&& self, bool fwd )
				{
					if ( !visited.emplace( blk ).second )
						return;
					for ( auto& prev : ( fwd ? blk->next : blk->prev ) )
						self( prev, self, fwd );
					worker( blk );
				};
				
				// If depth-first, start from entry point, iterate forward.
				//
				if constexpr ( T::exec_order == execution_order::serial_df )
				{
					rec( rtn->entry_point, rec, true );
				}
				// If breadth-first, start from each exit, iterate backward.
				//
				else
				{
					for ( const basic_block* exit : rtn->get_exits() )
						rec( make_mutable( exit ), rec, false );
				}
				break;
			}
			case execution_order::parallel:
			{
				// Invoke parallel transformation.
				//
				transform_parallel( rtn->explored_blocks, [ & ] ( const std::pair<const vip_t, basic_block*>& pair )
				{
					worker( pair.second );
				} );
				break;
			}
			case execution_order::parallel_bf:
			case execution_order::parallel_df:
			{
				// Get depth ordered list.
				//
				auto entries = rtn->get_depth_ordered_list( T::exec_order == execution_order::parallel_bf );

				// Begin segmentation loop.
				//
				auto it_begin = entries.begin();
				while ( it_begin != entries.end() )
				{
					// Find the last iterator with matching dependency.
					//
					auto it_end = it_begin;
					while ( it_end != entries.end() &&
							it_end->level_dependency == it_begin->level_dependency )
						it_end++;

					// Queue segment for work.
					//
					transform_parallel( make_range( it_begin, it_end ), [ & ] ( const routine::depth_placement& entry )
					{
						return worker( make_mutable( entry.block ) );
					} );

					// Continue search from next segment.
					//
					it_begin = it_end;
				}
				break;
			}
			default: 
				unreachable();
		}
		return n.load();
	}

	// Declares a generic pass interface that any optimization pass implements.
	// - Passes should be always default constructable.
	//
	template<execution_order order = execution_order::parallel>
	struct pass_interface
	{
		static constexpr execution_order exec_order = order;

		// Passes a single basic block through the optimizer, xblock will be set to true
		// if cross-block exploration is allowed.
		//
		virtual size_t pass( basic_block* blk, bool xblock = false ) = 0;

		// Passes every block through the optimizer with block refrences freely explorable,
		// returns the number of instances where this optimization was applied.
		//
		virtual size_t xpass( routine* rtn ) { return apply_pass( rtn, this ); }

		// Returns the name of the pass.
		//
		virtual std::string name() { return format::dynamic_type_name( *this ); }

		// Overload operator().
		//
		size_t operator()( basic_block* blk, bool xblock = false ) { return pass( blk, xblock ); }
		size_t operator()( routine* rtn ) { return xpass( rtn ); }
	};

	// Passes through each optimizer provided and returns the total number of optimizations applied.
	//
	template<typename... Tx>
	struct combine_pass;
	template<typename T>
	struct combine_pass<T> : T {};
	template<typename T1, typename... Tx>
	struct combine_pass<T1, Tx...> : pass_interface<execution_order::custom>
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			size_t n = T1{}.pass( blk, xblock );
			n += combine_pass<Tx...>{}.pass( blk, xblock );
			return n;
		}
		size_t xpass( routine* rtn ) override
		{
			size_t n = T1{}.xpass( rtn );
			n += combine_pass<Tx...>{}.xpass( rtn );
			return n;
		}
		std::string name() override { return "(" + T1{}.name() + " + " + combine_pass<Tx...>{}.name() + ")"; }
	};

	// Passes through first optimizer, if not no-op, passes through the rest.
	//
	template<typename T1, typename... Tx>
	struct conditional_pass : pass_interface<execution_order::custom>
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			if ( !xblock )
			{
				size_t n = T1{}.pass( blk, false );
				if ( n ) n += combine_pass<Tx...>{}.pass( blk, false );
				return n;
			}
			return T1{}.pass( blk, true );
		}
		size_t xpass( routine* rtn ) override
		{
			size_t n = T1{}.xpass( rtn );
			if ( n ) n += combine_pass<Tx...>{}.xpass( rtn );
			return n;
		}
		std::string name() override { return "conditional{" + T1{}.name() + " => " + combine_pass<Tx...>{}.name() + "}"; }
	};

	// Passes through each optimizer provided until the passes do not change the block.
	//
	template<typename... Tx>
	struct exhaust_pass : pass_interface<execution_order::custom>
	{
		// Simple looping until pass returns 0.
		//
		size_t pass( basic_block* blk, bool xblock = false ) override
		{ 
			size_t cnt = 0;
			while ( size_t n = combine_pass<Tx...>{}.pass( blk, xblock ) )
				cnt += n;
			return cnt;
		}
		size_t xpass( routine* rtn ) override
		{
			size_t cnt = 0;
			while ( size_t n = combine_pass<Tx...>{}.xpass( rtn ) )
				cnt += n;
			return cnt;
		}
		std::string name() override { return "exhaust{" + combine_pass<Tx...>{}.name() + "}"; }
	};

	// Specializes the pass logic depending on whether it's restricted or not.
	//
	template<typename opt_lblock, typename opt_xblock>
	struct specialize_pass : pass_interface<execution_order::custom>
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			return xblock ? opt_xblock{}.pass( blk, true ) : opt_lblock{}.pass( blk, false );
		}
		size_t xpass( routine* rtn ) override
		{
			return opt_xblock{}.xpass( rtn );
		}
		std::string name() override { return "specialize{local=" + opt_lblock{}.name() + ", cross=" + opt_xblock{}.name() + "}"; }
	};

	// Forces logic pass to ignore cross-block.
	//
	template<typename T>
	struct local_pass : T
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			return T::pass( blk, false );
		}
	};

	// Forces logic pass to return zero no matter what.
	//
	template<typename T>
	struct zero_pass : T
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			T::pass( blk, xblock );
			return 0;
		}
	};

	// No-op pass.
	//
	struct nop_pass : pass_interface<execution_order::custom>
	{
		size_t pass( basic_block* blk, bool xblock = false ) override { return 0; }
		size_t xpass( routine* rtn ) override { return 0; }
		std::string name() override { return "no-op"; }
	};

	// This wrapper spawns a new state of the given base type for each call
	// into pass and xpass letting the calls be const-qualified, can be used
	// for constexpr declarations.
	//
	template<typename T>
	struct spawn_state
	{
		// Imitate pass interface.
		//
		size_t pass( basic_block* blk, bool xblock = false ) const { return T{}.pass( blk, xblock ); }
		size_t xpass( routine* rtn ) const { return T{}.xpass( rtn ); }
		std::string name() { return T{}.name(); }

		// Overload operator().
		//
		size_t operator()( basic_block* blk, bool xblock = false ) const { return pass( blk, xblock ); }
		size_t operator()( routine* rtn ) const { return xpass( rtn ); }
	};

	// Dummy non-modifying wrapper.
	//
	template<typename T>
	struct nop_wrap : T
	{
		std::string name() override { return T{}.name(); }
	};

	// Used to profile the pass.
	//
	template<typename T>
	struct profile_pass : T
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			if ( !xblock )
				logger::log( "Block %08x => %-64s |", blk->entry_vip, T{}.name() );

			auto [cnt, time] = profile( [ & ] () { return T::pass( blk, xblock ); } );
			if ( !xblock )
				logger::log( " Took %-10s (N=%d).\n", time, cnt );
			return cnt;
		}

		size_t xpass( routine* rtn ) override
		{
			logger::log( "Routine => %-64s            |", T{}.name() );
			auto [cnt, time] = profile( [ & ] () { return T::xpass( rtn ); } );
			logger::log( " Took %-10s (N=%d).\n", time, cnt );
			return cnt;
		}
	};

	// Used to assert that the basic blocks contains updated analysis of the given type.
	//
	template<typename T>
	struct update_analysis : pass_interface<execution_order::custom>
	{
		size_t pass( basic_block* blk, bool xblock = false ) override
		{
			// Invoke get once and return.
			//
			auto& _ = blk->context.template get<T>();
			return 0;
		}

		size_t xpass( routine* rtn ) override 
		{
			// Enumerate over blocks and append to an update list if required.
			//
			std::vector<std::pair<basic_block*, T*>> update_list;
			update_list.reserve( rtn->num_blocks() );
			for ( auto& [vip, blk] : rtn->explored_blocks )
			{
				T& ref = blk->context.template get_raw<T>();
				if ( !ref.is_updated( blk ) )
					update_list.emplace_back( blk, &ref );
			}

			// Apply parallel transformation.
			//
			transform_parallel( update_list, [ ] ( const std::pair<basic_block*, T*>& pair )
			{
				pair.second->update_if( pair.first );
			} );
			return 0;
		}
	};

	// This wrapper applies a template modifier on each individual pass in the
	// given compound pass.
	//
	namespace impl
	{
		template<template<typename...> typename modifier, typename compound>
		struct apply_each_opt_t                                       { using type =     modifier<compound>; };

		template<template<typename...> typename modifier, typename compound>
		struct apply_each_opt_t<modifier, modifier<compound>>         { using type =     modifier<compound>; };

		template<template<typename...> typename modifier, typename... parts>
		struct apply_each_opt_t<modifier, spawn_state<parts...>>      { using type =     spawn_state<typename apply_each_opt_t<modifier, parts>::type...>;  };

		template<template<typename...> typename modifier, typename... parts>
		struct apply_each_opt_t<modifier, exhaust_pass<parts...>>     { using type =    exhaust_pass<typename apply_each_opt_t<modifier, parts>::type...>;  };

		template<template<typename...> typename modifier, typename... parts>
		struct apply_each_opt_t<modifier, combine_pass<parts...>>     { using type =    combine_pass<typename apply_each_opt_t<modifier, parts>::type...>;  };

		template<template<typename...> typename modifier, typename... parts>
		struct apply_each_opt_t<modifier, specialize_pass<parts...>>  { using type = specialize_pass<typename apply_each_opt_t<modifier, parts>::type...>;  };

		template<template<typename...> typename modifier, typename... parts>
		struct apply_each_opt_t<modifier, conditional_pass<parts...>> { using type = conditional_pass<typename apply_each_opt_t<modifier, parts>::type...>; };
	};

	template<template<typename...> typename modifier, typename compound>
	using apply_each = typename impl::apply_each_opt_t<modifier, compound>::type;
};