#include <stdio.h>
#include <map>
#include "pin.H"
#include "kscope.h"

/*
 * The lock for I/O.
 */
PIN_LOCK fileLock;


FILE * trace;
FILE * memTrace;
FILE * CodePool; // record instruction's disasm as string pool

static ADDRINT StartAddr = 0xFFFFFFFF;
static ADDRINT EndAddr = 0xFFFFFFFF;

static bool RecordFlag = false;

static map<unsigned short, unsigned int> ThreadUidDic; // index every instructions
static map<unsigned short, MemOP> ThreadReadMemDic; // index every instructions
static map<unsigned short, MemOP> ThreadWriteMemDic; // index every instructions

// Record a memory read record
VOID rec_mem_read( VOID * addr, UINT32 len )
{
	GetLock(&fileLock, 1);
	THREADID tid = ( PIN_ThreadId() & 0xFFFF );
	MemOP ReadBuffer;
	ReadBuffer.type = 'R';
	ReadBuffer.len = len;
	ReadBuffer.tid = tid;
	ReadBuffer.addr = addr;
	ReadBuffer.uid = ThreadUidDic[ReadBuffer.tid];
	if ( len > 4 ) // ignore this one!
	{
		ReleaseLock(&fileLock);
		return;
	}

	if ( len == 1 )
		PIN_SafeCopy(&(ReadBuffer.content), static_cast<UINT8*>(addr), 1);
	else if ( len == 2 )
		PIN_SafeCopy(&(ReadBuffer.content), static_cast<UINT16*>(addr), 2);
	else
		PIN_SafeCopy(&(ReadBuffer.content), static_cast<UINT32*>(addr), 4);

	ThreadReadMemDic[tid] = ReadBuffer;  // ���п���

	if ( 1 != fwrite( &ReadBuffer, sizeof(MemOP), 1, memTrace ) )
	{
		puts("write mem error\n");
		exit(0);
	}
	ReleaseLock(&fileLock);
}

// Record a memory write record
VOID rec_mem_write( VOID * addr, UINT32 len )
{
	GetLock(&fileLock, 1);
	THREADID tid = ( PIN_ThreadId() & 0xFFFF );
	MemOP WriteBuffer; // put this into stack for multi thread!
	WriteBuffer.addr = addr;
	WriteBuffer.tid = tid;
	WriteBuffer.type = 'W';
	WriteBuffer.len = len;
	WriteBuffer.uid = ThreadUidDic[WriteBuffer.tid];
	ThreadWriteMemDic[tid] = WriteBuffer;
	ReleaseLock(&fileLock);
}

VOID rec_mem_write_content()
{
	THREADID tid = ( PIN_ThreadId() & 0xFFFF );
	if ( ThreadWriteMemDic[tid].len > 4 ) // ignore this one!
		return;

	GetLock(&fileLock, 1);
	if ( ThreadWriteMemDic[tid].len == 1 )
		PIN_SafeCopy(&(ThreadWriteMemDic[tid].content), static_cast<UINT8*>(ThreadWriteMemDic[tid].addr), 1);
	else if ( ThreadWriteMemDic[tid].len == 2 )
		PIN_SafeCopy(&(ThreadWriteMemDic[tid].content), static_cast<UINT16*>(ThreadWriteMemDic[tid].addr), 2);
	else if ( ThreadWriteMemDic[tid].len == 4 )
		PIN_SafeCopy(&(ThreadWriteMemDic[tid].content), static_cast<UINT32*>(ThreadWriteMemDic[tid].addr), 4);
	
	if ( 1 != fwrite( &ThreadWriteMemDic[tid], sizeof(MemOP), 1, memTrace ) )
	{
		puts("write mem error\n");
		exit(0);
	}
	ReleaseLock(&fileLock);
}


VOID printip( const CONTEXT * const ctxt )
{
	GetLock(&fileLock, 1);
	static RegS IpBuffer;
	IpBuffer.eax = PIN_GetContextReg( ctxt, REG_EAX );
	IpBuffer.ebx = PIN_GetContextReg( ctxt, REG_EBX );
	IpBuffer.ecx = PIN_GetContextReg( ctxt, REG_ECX );
	IpBuffer.edx = PIN_GetContextReg( ctxt, REG_EDX );
	IpBuffer.edi = PIN_GetContextReg( ctxt, REG_EDI );
	IpBuffer.esi = PIN_GetContextReg( ctxt, REG_ESI );
	IpBuffer.ebp = PIN_GetContextReg( ctxt, REG_EBP );
	IpBuffer.esp = PIN_GetContextReg( ctxt, REG_ESP );
	IpBuffer.ip = PIN_GetContextReg( ctxt, REG_INST_PTR );
	IpBuffer.id = PIN_ThreadId();

	if ( ThreadUidDic.find(IpBuffer.id) != ThreadUidDic.end() )
		++ThreadUidDic[IpBuffer.id];
	else
		ThreadUidDic[IpBuffer.id] = 1;
	IpBuffer.uid = ThreadUidDic[IpBuffer.id];
	
/*
    out << "ss:    " << PIN_GetContextReg( ctxt, REG_SEG_SS ) << endl;
    out << "cs:    " << PIN_GetContextReg( ctxt, REG_SEG_CS ) << endl;
    out << "ds:    " << PIN_GetContextReg( ctxt, REG_SEG_DS ) << endl;
    out << "es:    " << PIN_GetContextReg( ctxt, REG_SEG_ES ) << endl;
    out << "fs:    " << PIN_GetContextReg( ctxt, REG_SEG_FS ) << endl;
    out << "gs:    " << PIN_GetContextReg( ctxt, REG_SEG_GS ) << endl;
    out << "gflags:" << PIN_GetContextReg( ctxt, REG_GFLAGS ) << endl;
*/
	if ( 1 != fwrite( &IpBuffer, sizeof(RegS), 1, trace ) )
	{
		puts("write ip error\n");
		exit(0); 
	}

	ReleaseLock(&fileLock);
}


VOID insert_mem_trace(INS ins)
{
    if (INS_IsMemoryWrite(ins))
    {

		INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(rec_mem_write), IARG_MEMORYWRITE_EA, IARG_MEMORYWRITE_SIZE, IARG_END);

		if (INS_HasFallThrough(ins))
        {
            INS_InsertPredicatedCall(ins, IPOINT_AFTER, AFUNPTR(rec_mem_write_content), IARG_MEMORYWRITE_SIZE, IARG_END);
        }
        if (INS_IsBranchOrCall(ins))
        {
            INS_InsertPredicatedCall(ins, IPOINT_TAKEN_BRANCH, AFUNPTR(rec_mem_write_content), IARG_MEMORYWRITE_SIZE, IARG_END);
        }
    }
	
    if ( INS_HasMemoryRead2(ins) )
    {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(rec_mem_read), IARG_MEMORYREAD2_EA, IARG_MEMORYREAD_SIZE, IARG_END);
    }

	if ( INS_IsMemoryRead(ins) )
    {
		INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(rec_mem_read), IARG_MEMORYREAD_EA, IARG_MEMORYREAD_SIZE, IARG_END);
    }

}


// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
	ADDRINT pc = INS_Address (ins);
    if ( pc == StartAddr )
    	RecordFlag = true;
    if ( pc == EndAddr )
    	RecordFlag = false;

	if ( RecordFlag )
	{
		fprintf( CodePool, "%08x|%s\n", pc, INS_Disassemble(ins).c_str() );

		// Insert a call to printip before every instruction, and pass it the IP
		INS_InsertCall( ins, IPOINT_BEFORE, (AFUNPTR)printip, IARG_CONTEXT, IARG_END );
		insert_mem_trace(ins);
	}
}


// This function is called when the application exits
VOID Fini(INT32 code, VOID *v)
{
	fclose( CodePool );
    fclose( memTrace );
	fclose( trace );
	puts("--FINI--\n");
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    PIN_ERROR("This Pintool prints the IPs of every instruction executed\n" 
              + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

bool init_config()
{
	trace = fopen("data/itrace.out", "w+b");
	CodePool = fopen("data/InstPool.out", "w");
	memTrace = fopen("data/memTrace.out", "w+b");

	printf( "Start Address:" );
	scanf( "%08x", &StartAddr );
	printf( "End Address:" );
	scanf( "%08x", &EndAddr );
	printf( "Start:%08x\tEnd:%08x\n", StartAddr, EndAddr );

	return ( trace && CodePool && memTrace );
}

int kaleidoscope(int argc, char * argv[])
{
    // Initialize pin
    if ( PIN_Init(argc, argv) )
		return Usage();

	if ( !init_config() )
		return -1;

	// Register Instruction to be called to instrument instructions
    INS_AddInstrumentFunction(Instruction, 0);

    // Register Fini to be called when the application exits
    PIN_AddFiniFunction(Fini, 0);
    
    // Start the program, never returns
    PIN_StartProgram();
    
    return 0;
}