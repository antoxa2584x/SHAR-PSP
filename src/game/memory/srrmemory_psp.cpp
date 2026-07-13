//=============================================================================
// srrmemory_psp.cpp — PSP implementation of the GMA_* placement operators.
//
// The full game routes each GameMemoryAllocator tag to a dedicated radMemory
// heap managed by HeapManager. The PSP port has a single ~22MB arena, so the
// tag is advisory only: every `new(GMA_...)` allocation goes to the default
// heap (global operator new). Matching `delete` uses the global operator
// delete, so the pair is consistent. The placement deletes only run if a
// constructor throws (PSP builds with no exceptions), so they simply forward.
//=============================================================================
#include <memory/srrmemory.h>

void* operator new( size_t size, GameMemoryAllocator )   { return ::operator new( size ); }
void  operator delete( void* pMemory, GameMemoryAllocator ) { ::operator delete( pMemory ); }
void* operator new[]( size_t size, GameMemoryAllocator ) { return ::operator new[]( size ); }
void  operator delete[]( void* pMemory, GameMemoryAllocator ) { ::operator delete[]( pMemory ); }
