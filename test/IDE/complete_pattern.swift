

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PATTERN_IS_1 > %t.types.txt
// RUN: FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.types.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PATTERN_IS_2 > %t.types.txt
// RUN: FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.types.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PATTERN_IS_3 > %t.types.txt
// RUN: FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.types.txt


// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PATTERN_IS_GENERIC_1 > %t.types.txt
// RUN: FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.types.txt
// RUN: FileCheck %s -check-prefix=PATTERN_IS_GENERIC_1 < %t.types.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PATTERN_IS_GENERIC_2 > %t.types.txt
// RUN: FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.types.txt
// RUN: FileCheck %s -check-prefix=PATTERN_IS_GENERIC_2 < %t.types.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=AFTER_PATTERN_IS | FileCheck %s -check-prefix=AFTER_PATTERN_IS

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=MULTI_PATTERN_1 | FileCheck %s -check-prefix=MULTI_PATTERN_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=MULTI_PATTERN_2 | FileCheck %s -check-prefix=MULTI_PATTERN_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=MULTI_PATTERN_3 | FileCheck %s -check-prefix=MULTI_PATTERN_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=MULTI_PATTERN_4 | FileCheck %s -check-prefix=MULTI_PATTERN_4


//===--- Helper types that are used in this test

struct FooStruct {
}

var fooObject : FooStruct

func fooFunc() -> FooStruct {
  return fooObject
}

enum FooEnum {
}

class FooClass {
}

protocol FooProtocol {
  var fooInstanceVar : Int
  typealias FooTypeAlias1
  func fooInstanceFunc0() -> Double
  func fooInstanceFunc1(a: Int) -> Double
  subscript(i: Int) -> Double
}

protocol BarProtocol {
  var barInstanceVar : Int
  typealias BarTypeAlias1
  func barInstanceFunc0() -> Double
  func barInstanceFunc1(a: Int) -> Double
}

typealias FooTypealias = Int

// GLOBAL_NEGATIVE-NOT: fooObject
// GLOBAL_NEGATIVE-NOT: fooFunc

//===---
//===--- Test that we don't try to suggest anything where pattern-atom is expected.
//===---

var #^PATTERN_ATOM_1^#
var (#^PATTERN_ATOM_2^#
var (a, #^PATTERN_ATOM_3^#
var (a #^PATTERN_ATOM_4^#
var ((#^PATTERN_ATOM_5^#
var ((a, b), #^PATTERN_ATOM_6^#

//===---
//===--- Test that we complete the type in 'is' pattern.
//===---

func patternIs1(x: FooClass) {
  switch x {
  case is #^PATTERN_IS_1^#
  }
}

func patternIs2() {
  switch unknown_var {
  case is #^PATTERN_IS_2^#
  }
}

func patternIs3() {
  switch {
  case is #^PATTERN_IS_3^#
  }
}

//===--- Test that we include types from generic parameter lists.

func patternIsGeneric1<
    GenericFoo : FooProtocol,
    GenericBar : FooProtocol & BarProtocol,
    GenericBaz>(x: FooClass) {
  switch x {
  case is #^PATTERN_IS_GENERIC_1^#
  }
}

// PATTERN_IS_GENERIC_1: Begin completions
// Generic parameters of the function.
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}
// PATTERN_IS_GENERIC_1: End completions

struct PatternIsGeneric2<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  func patternIsGeneric2<
      GenericFoo : FooProtocol,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(x: FooClass) {
    switch x {
    case is #^PATTERN_IS_GENERIC_2^#
    }
  }
}

// PATTERN_IS_GENERIC_2: Begin completions
// Generic parameters of the struct.
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/CurrNominal: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/CurrNominal: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/CurrNominal: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}
// Generic parameters of the function.
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2: End completions


// rdar://21174713
// AFTER_PATTERN_IS: Begin completions
func test<T>(x: T) {
  switch T.self {
  case is Int.Type:
    #^AFTER_PATTERN_IS^#
  }
}

func test_multiple_patterns1(x: Int) {
    switch (x, x) {
    case (0, let a), #^MULTI_PATTERN_1^#
    }
}

// MULTI_PATTERN_1: Begin completions
// MULTI_PATTERN_1-NOT: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_1-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}
// MULTI_PATTERN_1: End completions

func test_multiple_patterns2(x: Int) {
    switch (x, x) {
    case (0, let a), (let a, 0):
        #^MULTI_PATTERN_2^#
    }
}

// MULTI_PATTERN_2: Begin completions
// MULTI_PATTERN_2-DAG: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_2-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}
// MULTI_PATTERN_2: End completions

func test_multiple_patterns3(x: Int) {
    switch (x, x) {
    case (0, let a), (let b, 0):
        #^MULTI_PATTERN_3^#
    }
}

// MULTI_PATTERN_3: Begin completions
// MULTI_PATTERN_3-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}
// MULTI_PATTERN_3: End completions

func test_multiple_patterns4(x: Int) {
    switch (x, x) {
    case (0, let a) where #^MULTI_PATTERN_4^#
        
    }
}

// MULTI_PATTERN_4: Begin completions
// MULTI_PATTERN_4-DAG: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_4-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}
// MULTI_PATTERN_4: End completions

