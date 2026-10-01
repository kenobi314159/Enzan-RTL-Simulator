/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * The SystemVerilog additions IEEE 1800 makes to the Verilog Procedural Interface.
 *
 * The object types, relationships and properties an application uses to navigate a
 * SystemVerilog design, with the standard's numbers (see `vpi_user.h` for why those numbers
 * are not Enzan's to choose). The assertion and coverage interfaces of IEEE 1800 chapter 39
 * and 40 are not declared: Enzan has no assertions or coverage to report on (DESIGN_GUIDE.md
 * section 18, M9), and declaring routines nothing implements would only move the failure from
 * an application's compiler to its run.
 */

#ifndef ENZAN_SV_VPI_USER_H
#define ENZAN_SV_VPI_USER_H

#ifndef SV_VPI_USER_H
#define SV_VPI_USER_H

#include "vpi_user.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------
 * Object types
 * ------------------------------------------------------------------------------------------ */

#define vpiPackage 600
#define vpiInterface 601
#define vpiProgram 602
#define vpiInterfaceArray 603
#define vpiProgramArray 604
#define vpiTypespec 605
#define vpiModport 606
#define vpiInterfaceTfDecl 607
#define vpiRefObj 608
#define vpiTypeParameter 609

/* Variables. */
#define vpiVarBit vpiRegBit
#define vpiLongIntVar 610
#define vpiShortIntVar 611
#define vpiIntVar 612
#define vpiShortRealVar 613
#define vpiByteVar 614
#define vpiClassVar 615
#define vpiStringVar 616
#define vpiEnumVar 617
#define vpiStructVar 618
#define vpiUnionVar 619
#define vpiBitVar 620
#define vpiLogicVar vpiReg
#define vpiArrayVar vpiRegArray
#define vpiClassObj 621
#define vpiChandleVar 622
#define vpiPackedArrayVar 623
#define vpiVirtualInterfaceVar 728

/* Typespecs. */
#define vpiLongIntTypespec 625
#define vpiShortRealTypespec 626
#define vpiByteTypespec 627
#define vpiShortIntTypespec 628
#define vpiIntTypespec 629
#define vpiClassTypespec 630
#define vpiStringTypespec 631
#define vpiChandleTypespec 632
#define vpiEnumTypespec 633
#define vpiEnumConst 634
#define vpiIntegerTypespec 635
#define vpiTimeTypespec 636
#define vpiRealTypespec 637
#define vpiStructTypespec 638
#define vpiUnionTypespec 639
#define vpiBitTypespec 640
#define vpiLogicTypespec 641
#define vpiArrayTypespec 642
#define vpiVoidTypespec 643
#define vpiTypespecMember 644
#define vpiPackedArrayTypespec 692
#define vpiSequenceTypespec 696
#define vpiPropertyTypespec 697
#define vpiEventTypespec 698
#define vpiInterfaceTypespec 906

/* Behavioural objects. */
#define vpiClockingBlock 650
#define vpiClockingIODecl 651
#define vpiClassDefn 652
#define vpiConstraint 653
#define vpiConstraintOrdering 654
#define vpiDistItem 645
#define vpiAliasStmt 646
#define vpiThread 647
#define vpiMethodFuncCall 648
#define vpiMethodTaskCall 649
#define vpiReturn 666
#define vpiDoWhile 670
#define vpiOrderedWait 671
#define vpiWaitFork 672
#define vpiDisableFork 673
#define vpiExpectStmt 674
#define vpiForeachStmt 675
#define vpiReturnStmt 691
#define vpiFinal 676
#define vpiExtends 677
#define vpiDistribution 678
#define vpiBreak 684
#define vpiContinue 685

/* Nets. */
#define vpiArrayNet vpiNetArray
#define vpiEnumNet 680
#define vpiIntegerNet 681
#define vpiLogicNet vpiNet
#define vpiTimeNet 682
#define vpiUnionNet 525
#define vpiShortRealNet 526
#define vpiRealNet 527
#define vpiByteNet 528
#define vpiShortIntNet 529
#define vpiIntNet 530
#define vpiLongIntNet 531
#define vpiBitNet 532
#define vpiStructNet 683
#define vpiPackedArrayNet 693

/* ------------------------------------------------------------------------------------------
 * Relationships
 * ------------------------------------------------------------------------------------------ */

#define vpiActual 700
#define vpiTypedefAlias 701
#define vpiIndexTypespec 702
#define vpiBaseTypespec 703
#define vpiElemTypespec 704
#define vpiInputSkew 706
#define vpiOutputSkew 707
#define vpiGlobalClocking 708
#define vpiDefaultClocking 709
#define vpiDefaultDisableIff 710
#define vpiOrigin 713
#define vpiPrefix 714
#define vpiWith 715
#define vpiProperty 718
#define vpiValueRange 720
#define vpiPattern 721
#define vpiWeight 722
#define vpiTypedef 725
#define vpiImport 726
#define vpiDerivedClasses 727
#define vpiInterfaceDecl vpiVirtualInterfaceVar
#define vpiMethods 730
#define vpiSolveBefore 731
#define vpiSolveAfter 732
#define vpiWaitingProcesses 734
#define vpiMessages 735
#define vpiLoopVars 737
#define vpiConcurrentAssertions 740
#define vpiMatchItem 741
#define vpiMember 742
#define vpiElement 743
#define vpiAssertion 744
#define vpiInstance 745

/* ------------------------------------------------------------------------------------------
 * Properties
 * ------------------------------------------------------------------------------------------ */

#define vpiTop 600
#define vpiUnit 602
#define vpiJoinType 603
#define vpiJoin 0
#define vpiJoinNone 1
#define vpiJoinAny 2
#define vpiAccessType 604
#define vpiForkJoinAcc 1
#define vpiExternAcc 2
#define vpiDPIExportAcc 3
#define vpiDPIImportAcc 4
#define vpiArrayType 606
#define vpiStaticArray 1
#define vpiDynamicArray 2
#define vpiAssocArray 3
#define vpiQueueArray 4
#define vpiArrayMember 607
#define vpiIsRandomized 608
#define vpiLocalVarDecls 609
#define vpiRandType 610
#define vpiNotRand 1
#define vpiRand 2
#define vpiRandC 3
#define vpiPortType 611
#define vpiInterfacePort 1
#define vpiModportPort 2
#define vpiConstantVariable 612
#define vpiStructUnionMember 615
#define vpiVisibility 620
#define vpiPublicVis 1
#define vpiProtectedVis 2
#define vpiLocalVis 3
#define vpiOneStepConst 9
#define vpiUnboundedConst 10
#define vpiNullConst 11
#define vpiAlwaysType 624
#define vpiAlwaysComb 2
#define vpiAlwaysFF 3
#define vpiAlwaysLatch 4
#define vpiDistType 625
#define vpiEqualDist 1
#define vpiDivDist 2
#define vpiPacked 630
#define vpiTagged 632
#define vpiRef 6
#define vpiVirtual 635
#define vpiHasActual 636
#define vpiIsConstraintEnabled 638
#define vpiSoft 639
#define vpiClassType 640
#define vpiMailboxClass 1
#define vpiSemaphoreClass 2
#define vpiUserDefinedClass 3
#define vpiProcessClass 4
#define vpiMethod 645
#define vpiIsClockInferred 649
#define vpiIsDeferred 657
#define vpiIsFinal 670
#define vpiIsCoverSequence 659
#define vpiQualifier 650
#define vpiNoQualifier 0
#define vpiUniqueQualifier 1
#define vpiPriorityQualifier 2
#define vpiTaggedQualifier 4
#define vpiRandQualifier 8
#define vpiInsideQualifier 16
#define vpiInputEdge 651
#define vpiOutputEdge 652
#define vpiGeneric 653
#define vpiCompatibilityMode 654
#define vpiMode1364v1995 1
#define vpiMode1364v2001 2
#define vpiMode1364v2005 3
#define vpiMode1800v2005 4
#define vpiMode1800v2009 5
#define vpiPackedArrayMember 655
#define vpiStartLine 661
#define vpiColumn 662
#define vpiEndLine 663
#define vpiEndColumn 664
#define vpiAllocScheme 658
#define vpiAutomaticScheme 1
#define vpiDynamicScheme 2
#define vpiOtherScheme 3
#define vpiObjId 660
#define vpiDPIPure 665
#define vpiDPIContext 666
#define vpiDPICStr 667
#define vpiDPI 1
#define vpiDPIC 2
#define vpiDPICIdentifier 668
#define vpiIsModPort 669

/* Function types and validity, extending the values of vpi_user.h. */
#define vpiOtherFunc 6
#define vpiValidUnknown 2

/* ------------------------------------------------------------------------------------------
 * Callback reasons
 * ------------------------------------------------------------------------------------------ */

#define cbStartOfThread 600
#define cbEndOfThread 601
#define cbEnterThread 602
#define cbStartOfFrame 603
#define cbEndOfFrame 604
#define cbSizeChange 605
#define cbCreateObj 700
#define cbReclaimObj 701
#define cbEndOfObject 702

#ifdef __cplusplus
}
#endif

#endif /* SV_VPI_USER_H */

#endif /* ENZAN_SV_VPI_USER_H */
