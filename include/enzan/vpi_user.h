/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * The Verilog Procedural Interface, as IEEE 1364 and IEEE 1800 define it.
 *
 * This is the header a VPI application compiles against to be loaded into Enzan with
 * `--vpi` (DESIGN_GUIDE.md section 9). The constants, structures and routines are the
 * standard's: an application built against another simulator's copy of this header loads
 * into Enzan unchanged, which is the point of a standard interface, and every number here
 * MUST stay equal to the standard's or such an application silently asks for something else.
 *
 * Everything the standard declares is declared here, including what Enzan does not support,
 * because an application that names a constant must compile whether or not it ever uses
 * it. What is supported is section 9 of the design guide; a routine that is not fails and
 * reports why through `vpi_chk_error`.
 *
 * The SystemVerilog additions of IEEE 1800 are in `sv_vpi_user.h`.
 */

#ifndef ENZAN_VPI_USER_H
#define ENZAN_VPI_USER_H

/* The standard's own guard, so that an application which also includes another copy of this
 * header gets one set of definitions rather than two that conflict. */
#ifndef VPI_USER_H
#define VPI_USER_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------
 * Sized types
 * ------------------------------------------------------------------------------------------ */

#ifndef SVPI_TYPES
#define SVPI_TYPES
typedef int64_t PLI_INT64;
typedef uint64_t PLI_UINT64;
#endif

#ifndef PLI_TYPES
#define PLI_TYPES
typedef int PLI_INT32;
typedef unsigned int PLI_UINT32;
typedef short PLI_INT16;
typedef unsigned short PLI_UINT16;
typedef char PLI_BYTE8;
typedef unsigned char PLI_UBYTE8;
#endif

typedef PLI_UINT32* vpiHandle;

/* ------------------------------------------------------------------------------------------
 * Object types
 * ------------------------------------------------------------------------------------------ */

#define vpiAlways 1
#define vpiAssignStmt 2
#define vpiAssignment 3
#define vpiBegin 4
#define vpiCase 5
#define vpiCaseItem 6
#define vpiConstant 7
#define vpiContAssign 8
#define vpiDeassign 9
#define vpiDefParam 10
#define vpiDelayControl 11
#define vpiDisable 12
#define vpiEventControl 13
#define vpiEventStmt 14
#define vpiFor 15
#define vpiForce 16
#define vpiForever 17
#define vpiFork 18
#define vpiFuncCall 19
#define vpiFunction 20
#define vpiGate 21
#define vpiIf 22
#define vpiIfElse 23
#define vpiInitial 24
#define vpiIntegerVar 25
#define vpiInterModPath 26
#define vpiIterator 27
#define vpiIODecl 28
#define vpiMemory 29
#define vpiMemoryWord 30
#define vpiModPath 31
#define vpiModule 32
#define vpiNamedBegin 33
#define vpiNamedEvent 34
#define vpiNamedFork 35
#define vpiNet 36
#define vpiNetBit 37
#define vpiNullStmt 38
#define vpiOperation 39
#define vpiParamAssign 40
#define vpiParameter 41
#define vpiPartSelect 42
#define vpiPathTerm 43
#define vpiPort 44
#define vpiPortBit 45
#define vpiPrimTerm 46
#define vpiRealVar 47
#define vpiReg 48
#define vpiRegBit 49
#define vpiRelease 50
#define vpiRepeat 51
#define vpiRepeatControl 52
#define vpiSchedEvent 53
#define vpiSpecParam 54
#define vpiSwitch 55
#define vpiSysFuncCall 56
#define vpiSysTaskCall 57
#define vpiTableEntry 58
#define vpiTask 59
#define vpiTaskCall 60
#define vpiTchk 61
#define vpiTchkTerm 62
#define vpiTimeVar 63
#define vpiTimeQueue 64
#define vpiUdp 65
#define vpiUdpDefn 66
#define vpiUserSystf 67
#define vpiVarSelect 68
#define vpiWait 69
#define vpiWhile 70

#define vpiAttribute 105
#define vpiBitSelect 106
#define vpiCallback 107
#define vpiDelayTerm 108
#define vpiDelayDevice 109
#define vpiFrame 110
#define vpiGateArray 111
#define vpiModuleArray 112
#define vpiPrimitiveArray 113
#define vpiNetArray 114
#define vpiRange 115
#define vpiRegArray 116
#define vpiSwitchArray 117
#define vpiUdpArray 118
#define vpiContAssignBit 128
#define vpiNamedEventArray 129
#define vpiIndexedPartSelect 130
#define vpiGenScopeArray 133
#define vpiGenScope 134
#define vpiGenVar 135

/* ------------------------------------------------------------------------------------------
 * Relationships
 * ------------------------------------------------------------------------------------------ */

/* One to one. */
#define vpiCondition 71
#define vpiDelay 72
#define vpiElseStmt 73
#define vpiForIncStmt 74
#define vpiForInitStmt 75
#define vpiHighConn 76
#define vpiLhs 77
#define vpiIndex 78
#define vpiLeftRange 79
#define vpiLowConn 80
#define vpiParent 81
#define vpiRhs 82
#define vpiRightRange 83
#define vpiScope 84
#define vpiSysTfCall 85
#define vpiTchkDataTerm 86
#define vpiTchkNotifier 87
#define vpiTchkRefTerm 88

/* One to many. */
#define vpiArgument 89
#define vpiBit 90
#define vpiDriver 91
#define vpiInternalScope 92
#define vpiLoad 93
#define vpiModDataPathIn 94
#define vpiModPathIn 95
#define vpiModPathOut 96
#define vpiOperand 97
#define vpiPortInst 98
#define vpiProcess 99
#define vpiVariables 100
#define vpiUse 101

/* Many to one. */
#define vpiExpr 102
#define vpiPrimitive 103
#define vpiStmt 104

#define vpiActiveTimeFormat 119
#define vpiInTerm 120
#define vpiInstanceArray 121
#define vpiLocalDriver 122
#define vpiLocalLoad 123
#define vpiOutTerm 124
#define vpiPorts 125
#define vpiSimNet 126
#define vpiTaskFunc 127
#define vpiBaseExpr 131
#define vpiWidthExpr 132
#define vpiAutomatics 136

/* ------------------------------------------------------------------------------------------
 * Properties, and the values some of them take
 * ------------------------------------------------------------------------------------------ */

#define vpiUndefined -1
#define vpiType 1
#define vpiName 2
#define vpiFullName 3
#define vpiSize 4
#define vpiFile 5
#define vpiLineNo 6
#define vpiTopModule 7
#define vpiCellInstance 8
#define vpiDefName 9
#define vpiProtected 10
#define vpiTimeUnit 11
#define vpiTimePrecision 12
#define vpiDefNetType 13
#define vpiUnconnDrive 14
#define vpiHighZ 1
#define vpiPull1 2
#define vpiPull0 3
#define vpiDefFile 15
#define vpiDefLineNo 16
#define vpiDefDelayMode 47
#define vpiDelayModeNone 1
#define vpiDelayModePath 2
#define vpiDelayModeDistrib 3
#define vpiDelayModeUnit 4
#define vpiDelayModeZero 5
#define vpiDelayModeMTM 6
#define vpiDefDecayTime 48

#define vpiScalar 17
#define vpiVector 18
#define vpiExplicitName 19
#define vpiDirection 20
#define vpiInput 1
#define vpiOutput 2
#define vpiInout 3
#define vpiMixedIO 4
#define vpiNoDirection 5
#define vpiConnByName 21

#define vpiNetType 22
#define vpiWire 1
#define vpiWand 2
#define vpiWor 3
#define vpiTri 4
#define vpiTri0 5
#define vpiTri1 6
#define vpiTriReg 7
#define vpiTriAnd 8
#define vpiTriOr 9
#define vpiSupply1 10
#define vpiSupply0 11
#define vpiNone 12
#define vpiUwire 13

#define vpiExplicitScalared 23
#define vpiExplicitVectored 24
#define vpiExpanded 25
#define vpiImplicitDecl 26
#define vpiChargeStrength 27
#define vpiArray 28
#define vpiPortIndex 29
#define vpiTermIndex 30
#define vpiStrength0 31
#define vpiStrength1 32

#define vpiPrimType 33
#define vpiAndPrim 1
#define vpiNandPrim 2
#define vpiNorPrim 3
#define vpiOrPrim 4
#define vpiXorPrim 5
#define vpiXnorPrim 6
#define vpiBufPrim 7
#define vpiNotPrim 8
#define vpiBufif0Prim 9
#define vpiBufif1Prim 10
#define vpiNotif0Prim 11
#define vpiNotif1Prim 12
#define vpiNmosPrim 13
#define vpiPmosPrim 14
#define vpiCmosPrim 15
#define vpiRnmosPrim 16
#define vpiRpmosPrim 17
#define vpiRcmosPrim 18
#define vpiRtranPrim 19
#define vpiRtranif0Prim 20
#define vpiRtranif1Prim 21
#define vpiTranPrim 22
#define vpiTranif0Prim 23
#define vpiTranif1Prim 24
#define vpiPullupPrim 25
#define vpiPulldownPrim 26
#define vpiSeqPrim 27
#define vpiCombPrim 28

#define vpiPolarity 34
#define vpiDataPolarity 35
#define vpiPositive 1
#define vpiNegative 2
#define vpiUnknown 3

#define vpiEdge 36
#define vpiNoEdge 0x00
#define vpiEdge01 0x01
#define vpiEdge10 0x02
#define vpiEdge0x 0x04
#define vpiEdgex1 0x08
#define vpiEdge1x 0x10
#define vpiEdgex0 0x20
#define vpiPosedge (vpiEdgex1 | vpiEdge01 | vpiEdge0x)
#define vpiNegedge (vpiEdgex0 | vpiEdge10 | vpiEdge1x)
#define vpiAnyEdge (vpiPosedge | vpiNegedge)

#define vpiPathType 37
#define vpiPathFull 1
#define vpiPathParallel 2

#define vpiTchkType 38
#define vpiSetup 1
#define vpiHold 2
#define vpiPeriod 3
#define vpiWidth 4
#define vpiSkew 5
#define vpiRecovery 6
#define vpiNoChange 7
#define vpiSetupHold 8
#define vpiFullskew 9
#define vpiRecrem 10
#define vpiRemoval 11
#define vpiTimeskew 12

#define vpiOpType 39
#define vpiMinusOp 1
#define vpiPlusOp 2
#define vpiNotOp 3
#define vpiBitNegOp 4
#define vpiUnaryAndOp 5
#define vpiUnaryNandOp 6
#define vpiUnaryOrOp 7
#define vpiUnaryNorOp 8
#define vpiUnaryXorOp 9
#define vpiUnaryXNorOp 10
#define vpiSubOp 11
#define vpiDivOp 12
#define vpiModOp 13
#define vpiEqOp 14
#define vpiNeqOp 15
#define vpiCaseEqOp 16
#define vpiCaseNeqOp 17
#define vpiGtOp 18
#define vpiGeOp 19
#define vpiLtOp 20
#define vpiLeOp 21
#define vpiLShiftOp 22
#define vpiRShiftOp 23
#define vpiAddOp 24
#define vpiMultOp 25
#define vpiLogAndOp 26
#define vpiLogOrOp 27
#define vpiBitAndOp 28
#define vpiBitOrOp 29
#define vpiBitXorOp 30
#define vpiBitXNorOp 31
#define vpiBitXnorOp vpiBitXNorOp
#define vpiConditionOp 32
#define vpiConcatOp 33
#define vpiMultiConcatOp 34
#define vpiEventOrOp 35
#define vpiNullOp 36
#define vpiListOp 37
#define vpiMinTypMaxOp 38
#define vpiPosedgeOp 39
#define vpiNegedgeOp 40
#define vpiArithLShiftOp 41
#define vpiArithRShiftOp 42
#define vpiPowerOp 43

#define vpiConstType 40
#define vpiDecConst 1
#define vpiRealConst 2
#define vpiBinaryConst 3
#define vpiOctConst 4
#define vpiHexConst 5
#define vpiStringConst 6
#define vpiIntConst 7
#define vpiTimeConst 8

#define vpiBlocking 41
#define vpiCaseType 42
#define vpiCaseExact 1
#define vpiCaseX 2
#define vpiCaseZ 3
#define vpiNetDeclAssign 43

#define vpiFuncType 44
#define vpiIntFunc 1
#define vpiRealFunc 2
#define vpiTimeFunc 3
#define vpiSizedFunc 4
#define vpiSizedSignedFunc 5
#define vpiSysFuncType vpiFuncType
#define vpiSysFuncInt vpiIntFunc
#define vpiSysFuncReal vpiRealFunc
#define vpiSysFuncTime vpiTimeFunc
#define vpiSysFuncSized vpiSizedFunc

#define vpiUserDefn 45
#define vpiScheduled 46
#define vpiActive 49
#define vpiAutomatic 50
#define vpiCell 51
#define vpiConfig 52
#define vpiConstantSelect 53
#define vpiDecompile 54
#define vpiDefAttribute 55

#define vpiDelayType 56
#define vpiModPathDelay 1
#define vpiInterModPathDelay 2
#define vpiMIPDelay 3

#define vpiIteratorType 57
#define vpiLibrary 58
#define vpiMultiArray 59
#define vpiOffset 60
#define vpiResolvedNetType 61
#define vpiSaveRestartID 62
#define vpiSaveRestartLocation 63
#define vpiValid 64
#define vpiValidFalse 0
#define vpiValidTrue 1
#define vpiSigned 65
#define vpiLocalParam 70
#define vpiModPathHasIfNone 71
#define vpiIndexedPartSelectType 72
#define vpiPosIndexed 1
#define vpiNegIndexed 2
#define vpiIsMemory 73
#define vpiIsProtected 74

/* ------------------------------------------------------------------------------------------
 * vpi_control operations
 * ------------------------------------------------------------------------------------------ */

#define vpiStop 66
#define vpiFinish 67
#define vpiReset 68
#define vpiSetInteractiveScope 69

/* ------------------------------------------------------------------------------------------
 * Multichannel descriptors
 * ------------------------------------------------------------------------------------------ */

#define VPI_MCD_STDOUT 0x00000001

/* ------------------------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------------------------ */

#ifndef VPI_TIME
#define VPI_TIME

typedef struct t_vpi_time {
    PLI_INT32 type; /* vpiScaledRealTime, vpiSimTime or vpiSuppressTime */
    PLI_UINT32 high, low;
    double real;
} s_vpi_time, *p_vpi_time;

#define vpiScaledRealTime 1
#define vpiSimTime 2
#define vpiSuppressTime 3
#endif

typedef struct t_vpi_delay {
    struct t_vpi_time* da;
    PLI_INT32 no_of_delays;
    PLI_INT32 time_type;
    PLI_INT32 mtm_flag;
    PLI_INT32 append_flag;
    PLI_INT32 pulsere_flag;
} s_vpi_delay, *p_vpi_delay;

/* ------------------------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------------------------ */

/* One 32-bit slice of a four-state vector, in the same two-plane encoding Enzan stores
 * values in (DESIGN_GUIDE.md section 3.6): aval/bval 00 is 0, 10 is 1, 01 is Z and 11 is X. */
#ifndef VPI_VECVAL
#define VPI_VECVAL

typedef struct t_vpi_vecval {
    PLI_INT32 aval, bval;
} s_vpi_vecval, *p_vpi_vecval;
#endif

typedef struct t_vpi_strengthval {
    PLI_INT32 logic;
    PLI_INT32 s0, s1;
} s_vpi_strengthval, *p_vpi_strengthval;

#define vpiSupplyDrive 0x80
#define vpiStrongDrive 0x40
#define vpiPullDrive 0x20
#define vpiWeakDrive 0x08
#define vpiLargeCharge 0x10
#define vpiMediumCharge 0x04
#define vpiSmallCharge 0x02
#define vpiHiZ 0x01

typedef struct t_vpi_value {
    PLI_INT32 format;

    union {
        PLI_BYTE8* str;
        PLI_INT32 scalar;
        PLI_INT32 integer;
        double real;
        struct t_vpi_time* time;
        struct t_vpi_vecval* vector;
        struct t_vpi_strengthval* strength;
        PLI_BYTE8* misc;
    } value;
} s_vpi_value, *p_vpi_value;

typedef struct t_vpi_arrayvalue {
    PLI_UINT32 format;
    PLI_UINT32 flags;

    union {
        PLI_INT32* integers;
        PLI_INT16* shortints;
        PLI_INT64* longints;
        PLI_BYTE8* rawvals;
        struct t_vpi_vecval* vectors;
        struct t_vpi_time* times;
        double* reals;
        float* shortreals;
    } value;
} s_vpi_arrayvalue, *p_vpi_arrayvalue;

/* Value formats. */
#define vpiBinStrVal 1
#define vpiOctStrVal 2
#define vpiDecStrVal 3
#define vpiHexStrVal 4
#define vpiScalarVal 5
#define vpiIntVal 6
#define vpiRealVal 7
#define vpiStringVal 8
#define vpiVectorVal 9
#define vpiStrengthVal 10
#define vpiTimeVal 11
#define vpiObjTypeVal 12
#define vpiSuppressVal 13
#define vpiShortIntVal 14
#define vpiLongIntVal 15
#define vpiShortRealVal 16
#define vpiRawTwoStateVal 17
#define vpiRawFourStateVal 18

/* Delay modes and flags of vpi_put_value. */
#define vpiNoDelay 1
#define vpiInertialDelay 2
#define vpiTransportDelay 3
#define vpiPureTransportDelay 4
#define vpiForceFlag 5
#define vpiReleaseFlag 6
#define vpiCancelEvent 7
#define vpiReturnEvent 0x1000
#define vpiUserAllocFlag 0x2000
#define vpiOneValue 0x4000
#define vpiPropagateOff 0x8000

/* Scalar values. */
#define vpi0 0
#define vpi1 1
#define vpiZ 2
#define vpiX 3
#define vpiH 4
#define vpiL 5
#define vpiDontCare 6

/* ------------------------------------------------------------------------------------------
 * User system tasks and functions
 * ------------------------------------------------------------------------------------------ */

typedef struct t_vpi_systf_data {
    PLI_INT32 type;
    PLI_INT32 sysfunctype;
    PLI_BYTE8* tfname;
    PLI_INT32 (*calltf)(PLI_BYTE8*);
    PLI_INT32 (*compiletf)(PLI_BYTE8*);
    PLI_INT32 (*sizetf)(PLI_BYTE8*);
    PLI_BYTE8* user_data;
} s_vpi_systf_data, *p_vpi_systf_data;

#define vpiSysTask 1
#define vpiSysFunc 2

/* ------------------------------------------------------------------------------------------
 * The simulator, and errors
 * ------------------------------------------------------------------------------------------ */

typedef struct t_vpi_vlog_info {
    PLI_INT32 argc;
    PLI_BYTE8** argv;
    PLI_BYTE8* product;
    PLI_BYTE8* version;
} s_vpi_vlog_info, *p_vpi_vlog_info;

typedef struct t_vpi_error_info {
    PLI_INT32 state;
    PLI_INT32 level;
    PLI_BYTE8* message;
    PLI_BYTE8* product;
    PLI_BYTE8* code;
    PLI_BYTE8* file;
    PLI_INT32 line;
} s_vpi_error_info, *p_vpi_error_info;

/* Error states. */
#define vpiCompile 1
#define vpiPLI 2
#define vpiRun 3

/* Error levels. */
#define vpiNotice 1
#define vpiWarning 2
#define vpiError 3
#define vpiSystem 4
#define vpiInternal 5

/* ------------------------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------------------------ */

typedef struct t_cb_data {
    PLI_INT32 reason;
    PLI_INT32 (*cb_rtn)(struct t_cb_data*);
    vpiHandle obj;
    p_vpi_time time;
    p_vpi_value value;
    PLI_INT32 index;
    PLI_BYTE8* user_data;
} s_cb_data, *p_cb_data;

#define cbValueChange 1
#define cbStmt 2
#define cbForce 3
#define cbRelease 4
#define cbAtStartOfSimTime 5
#define cbReadWriteSynch 6
#define cbReadOnlySynch 7
#define cbNextSimTime 8
#define cbAfterDelay 9
#define cbEndOfCompile 10
#define cbStartOfSimulation 11
#define cbEndOfSimulation 12
#define cbError 13
#define cbTchkViolation 14
#define cbStartOfSave 15
#define cbEndOfSave 16
#define cbStartOfRestart 17
#define cbEndOfRestart 18
#define cbStartOfReset 19
#define cbEndOfReset 20
#define cbEnterInteractive 21
#define cbExitInteractive 22
#define cbInteractiveScopeChange 23
#define cbUnresolvedSystf 24
#define cbAssign 25
#define cbDeassign 26
#define cbDisable 27
#define cbPLIError 28
#define cbSignal 29
#define cbNBASynch 30
#define cbAtEndOfSimTime 31

/* ------------------------------------------------------------------------------------------
 * Routines
 * ------------------------------------------------------------------------------------------ */

vpiHandle vpi_register_cb(p_cb_data cb_data_p);
PLI_INT32 vpi_remove_cb(vpiHandle cb_obj);
void vpi_get_cb_info(vpiHandle object, p_cb_data cb_data_p);
vpiHandle vpi_register_systf(p_vpi_systf_data systf_data_p);
void vpi_get_systf_info(vpiHandle object, p_vpi_systf_data systf_data_p);

vpiHandle vpi_handle_by_name(PLI_BYTE8* name, vpiHandle scope);
vpiHandle vpi_handle_by_index(vpiHandle object, PLI_INT32 indx);
vpiHandle vpi_handle_by_multi_index(vpiHandle obj, PLI_INT32 num_index, PLI_INT32* index_array);
vpiHandle vpi_handle(PLI_INT32 type, vpiHandle refHandle);
vpiHandle vpi_handle_multi(PLI_INT32 type, vpiHandle refHandle1, vpiHandle refHandle2, ...);
vpiHandle vpi_iterate(PLI_INT32 type, vpiHandle refHandle);
vpiHandle vpi_scan(vpiHandle iterator);

PLI_INT32 vpi_get(PLI_INT32 property, vpiHandle object);
PLI_INT64 vpi_get64(PLI_INT32 property, vpiHandle object);
PLI_BYTE8* vpi_get_str(PLI_INT32 property, vpiHandle object);

void vpi_get_delays(vpiHandle object, p_vpi_delay delay_p);
void vpi_put_delays(vpiHandle object, p_vpi_delay delay_p);

void vpi_get_value(vpiHandle expr, p_vpi_value value_p);
vpiHandle vpi_put_value(vpiHandle object, p_vpi_value value_p, p_vpi_time time_p, PLI_INT32 flags);
void vpi_get_value_array(vpiHandle expr, p_vpi_arrayvalue arrayvalue_p, PLI_INT32* index_p,
                         PLI_UINT32 num);
void vpi_put_value_array(vpiHandle object, p_vpi_arrayvalue arrayvalue_p, PLI_INT32* index_p,
                         PLI_UINT32 num);

void vpi_get_time(vpiHandle object, p_vpi_time time_p);

PLI_UINT32 vpi_mcd_open(PLI_BYTE8* fileName);
PLI_UINT32 vpi_mcd_close(PLI_UINT32 mcd);
PLI_BYTE8* vpi_mcd_name(PLI_UINT32 cd);
PLI_INT32 vpi_mcd_printf(PLI_UINT32 mcd, PLI_BYTE8* format, ...);
PLI_INT32 vpi_mcd_vprintf(PLI_UINT32 mcd, PLI_BYTE8* format, va_list ap);
PLI_INT32 vpi_mcd_flush(PLI_UINT32 mcd);
PLI_INT32 vpi_printf(PLI_BYTE8* format, ...);
PLI_INT32 vpi_vprintf(PLI_BYTE8* format, va_list ap);
PLI_INT32 vpi_flush(void);

PLI_INT32 vpi_compare_objects(vpiHandle object1, vpiHandle object2);
PLI_INT32 vpi_chk_error(p_vpi_error_info error_info_p);
PLI_INT32 vpi_free_object(vpiHandle object);
PLI_INT32 vpi_release_handle(vpiHandle object);
PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info vlog_info_p);

PLI_INT32 vpi_get_data(PLI_INT32 id, PLI_BYTE8* dataLoc, PLI_INT32 numOfBytes);
PLI_INT32 vpi_put_data(PLI_INT32 id, PLI_BYTE8* dataLoc, PLI_INT32 numOfBytes);
void* vpi_get_userdata(vpiHandle obj);
PLI_INT32 vpi_put_userdata(vpiHandle obj, void* userdata);

PLI_INT32 vpi_control(PLI_INT32 operation, ...);

/* What an application defines: the routines Enzan calls when it loads the library, ending
 * with a null pointer. */
extern void (*vlog_startup_routines[])(void);

#ifdef __cplusplus
}
#endif

#endif /* VPI_USER_H */

#endif /* ENZAN_VPI_USER_H */
