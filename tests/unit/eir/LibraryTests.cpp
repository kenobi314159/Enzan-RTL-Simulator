// SPDX-License-Identifier: GPL-3.0-or-later
//
// The store's whole job is to answer "has anything that matters changed?", and the dangerous
// answer is the false no: a library that hands back units analysed under different macros, or
// by a different translator, gives a simulation that runs and is wrong. So most of what is
// here is about what counts as a miss.

#include "eir/Library.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Type.h"

#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceLocation;
using enzan::SourceManager;
using enzan::eir::Design;
using enzan::eir::Library;
using enzan::eir::LibrarySource;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Type;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

class LibraryTest : public ::testing::Test {
protected:
    LibraryTest() : engine(sources) { engine.addConsumer(consumer); }

    /// Registers `text` as the contents of `name` and returns it as a library source.
    LibrarySource source(std::string_view name, std::string_view text) {
        const std::filesystem::path path = workspace / name;
        return {.path = path, .buffer = sources.addBuffer(path.string(), std::string(text))};
    }

    /// A design holding one module whose definition sits in `buffer`.
    static Design designWith(std::string_view name, BufferId buffer) {
        ModuleDefinition definition;
        definition.name = std::string(name);
        definition.sourceName = std::string(name);
        definition.location = SourceLocation(buffer, 0);
        definition.addObject(Object{.name = "clk", .type = Type::logic()});

        Design design;
        design.addModule(std::move(definition));
        return design;
    }

    [[nodiscard]] bool reported(DiagCode code) const {
        return std::ranges::any_of(consumer.received, [code](const Diagnostic& diagnostic) {
            return diagnostic.code == code;
        });
    }

    enzan::test::Workspace workspace;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

TEST_F(LibraryTest, CreatingMakesTheDirectoryAndNamesIt) {
    Library library;
    const std::filesystem::path path = workspace / "work";

    ASSERT_TRUE(library.create(path, {}, engine));
    EXPECT_TRUE(library.isOpen());
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_EQ(library.path(), path);
    EXPECT_EQ(library.name(), "work") << "an empty name takes the directory's own";
}

TEST_F(LibraryTest, ACreatedLibraryIsOpenedByName) {
    const std::filesystem::path path = workspace / "store";
    {
        Library made;
        ASSERT_TRUE(made.create(path, "acme", engine));
    }

    Library library;
    ASSERT_TRUE(library.open(path, engine));
    EXPECT_EQ(library.name(), "acme") << "the name comes from the library, not from the caller";
}

TEST_F(LibraryTest, ADirectoryThatWasNeverCreatedIsNotALibrary) {
    // Libraries are opt-in (8.2), so a directory that merely exists is a mistake to report
    // rather than a library to invent.
    const std::filesystem::path path = workspace / "work";
    std::filesystem::create_directories(path);

    Library library;
    EXPECT_FALSE(library.open(path, engine));
    EXPECT_TRUE(reported(DiagCode::NotALibrary));
}

TEST_F(LibraryTest, OpeningSomewhereThatDoesNotExistIsNotALibrary) {
    Library library;
    EXPECT_FALSE(library.open(workspace / "nowhere", engine));
    EXPECT_TRUE(reported(DiagCode::NotALibrary));
}

TEST_F(LibraryTest, RefusesAPathThatIsAFile) {
    workspace.write("work", "not a directory");

    Library library;
    EXPECT_FALSE(library.create(workspace / "work", {}, engine));
    EXPECT_FALSE(library.isOpen());
    EXPECT_TRUE(reported(DiagCode::LibraryNotADirectory));
}

TEST_F(LibraryTest, RefusesANameADesignCouldNotWrite) {
    Library library;
    EXPECT_FALSE(library.create(workspace / "work", "my-lib", engine));
    EXPECT_TRUE(reported(DiagCode::LibraryBadName));
}

TEST_F(LibraryTest, RefusesADirectoryWhoseNameIsNotAnIdentifier) {
    // The default name is the basename, so a directory nobody could name in a design has to
    // be told about rather than silently given a different name.
    Library library;
    EXPECT_FALSE(library.create(workspace / "my-lib", {}, engine));
    EXPECT_TRUE(reported(DiagCode::LibraryBadName));
}

TEST_F(LibraryTest, ANameIsAnIdentifier) {
    EXPECT_TRUE(Library::isValidName("work"));
    EXPECT_TRUE(Library::isValidName("_ieee2008"));
    EXPECT_FALSE(Library::isValidName(""));
    EXPECT_FALSE(Library::isValidName("2fast"));
    EXPECT_FALSE(Library::isValidName("has.dot")) << "a dot is how a unit name is qualified";
    EXPECT_FALSE(Library::isValidName("has space"));
}

TEST_F(LibraryTest, AClosedLibraryNeitherLoadsNorStores) {
    Library library;
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const Design design = designWith("a", file.buffer);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
    EXPECT_FALSE(library.store({&file, 1}, sources, design, engine));
}

// ---------------------------------------------------------------------------
// Storing and reusing
// ---------------------------------------------------------------------------

/// The time unit the tests analyse in, as a power of ten of a second.
constexpr std::int8_t Picoseconds = -12;

class StoredLibraryTest : public LibraryTest {
protected:
    StoredLibraryTest() { EXPECT_TRUE(library.create(workspace / "work", {}, engine)); }

    Library library;
};

TEST_F(StoredLibraryTest, WhatWasStoredComesBack) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    Design loaded;
    ASSERT_TRUE(library.load({&file, 1}, sources, loaded));
    EXPECT_EQ(library.entriesLoaded(), 1U);
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded.module(0).name, "a");
    EXPECT_EQ(loaded.module(0).objects.size(), 1U);
}

TEST_F(StoredLibraryTest, ALocationPointsBackIntoTheCallersBuffer) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    Design loaded;
    ASSERT_TRUE(library.load({&file, 1}, sources, loaded));
    EXPECT_EQ(loaded.module(0).location, SourceLocation(file.buffer, 0));
}

TEST_F(StoredLibraryTest, NothingStoredIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST_F(StoredLibraryTest, LoadingNothingIsAMissRatherThanAnEmptyHit) {
    library.setOptions({}, {}, {}, Picoseconds);

    Design loaded;
    EXPECT_FALSE(library.load({}, sources, loaded));
}

TEST_F(StoredLibraryTest, ChangedTextIsAMiss) {
    const LibrarySource before = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&before, 1}, sources, designWith("a", before.buffer), engine));

    const LibrarySource after = source("a.sv", "module a; wire w; endmodule\n");
    Design loaded;
    EXPECT_FALSE(library.load({&after, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, TheSameTextUnderAnotherNameIsAMiss) {
    // Two files with identical contents are still two units, and a diagnostic about one must
    // name the right file.
    const LibrarySource first = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&first, 1}, sources, designWith("a", first.buffer), engine));

    const LibrarySource second = source("b.sv", "module a; endmodule\n");
    Design loaded;
    EXPECT_FALSE(library.load({&second, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AChangedDefineIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::string> before{"WIDTH=8"};
    library.setOptions(before, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::string> after{"WIDTH=16"};
    library.setOptions(after, {}, {}, Picoseconds);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AChangedIncludePathIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::filesystem::path> before{"include"};
    library.setOptions({}, before, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::filesystem::path> after{"include", "vendor"};
    library.setOptions({}, after, {}, Picoseconds);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AChangedTimeUnitIsAMiss) {
    // A delay is stored as a count of the unit, so `#5` in picoseconds is not `#5` in
    // nanoseconds.
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    library.setOptions({}, {}, {}, -9);
    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, TheTimeUnitIsRecordedForWhoeverReadsTheLibrary) {
    EXPECT_FALSE(library.timeUnit().has_value()) << "a new library holds no units, so no unit";

    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, -9);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    EXPECT_EQ(library.timeUnit(), std::optional<std::int8_t>(-9));

    Library reader;
    ASSERT_TRUE(reader.open(workspace / "work", engine));
    EXPECT_EQ(reader.timeUnit(), std::optional<std::int8_t>(-9));
}

TEST_F(StoredLibraryTest, ALibraryEmptiedOfItsEntriesHoldsNoTimeUnit) {
    // What `lib clean` leaves can be filled in another unit.
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, -9);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    for (const auto& entry : std::filesystem::directory_iterator(workspace / "work")) {
        if (entry.path().extension() == ".eirlib") std::filesystem::remove(entry.path());
    }
    EXPECT_FALSE(library.timeUnit().has_value());
}

TEST_F(StoredLibraryTest, TwoOptionsThatConcatenateAlikeAreDistinguished) {
    // `+define+AB` and `+define+A +define+B` must not hash to the same key, which is what a
    // digest with no separator between fields would do.
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::string> joined{"AB"};
    library.setOptions(joined, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::string> split{"A", "B"};
    library.setOptions(split, {}, {}, Picoseconds);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AParameterOverrideIsPartOfTheKey) {
    // slang applies `-G` while analysing, so a unit analysed with an override is not the one
    // analysed without, and an override must not hash as the define spelt like it.
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::string> width{"WIDTH=8"};
    library.setOptions({}, {}, width, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    library.setOptions({}, {}, {}, Picoseconds);
    Design without;
    EXPECT_FALSE(library.load({&file, 1}, sources, without));

    library.setOptions(width, {}, {}, Picoseconds);
    Design asDefine;
    EXPECT_FALSE(library.load({&file, 1}, sources, asDefine));

    library.setOptions({}, {}, width, Picoseconds);
    Design same;
    EXPECT_TRUE(library.load({&file, 1}, sources, same));
}

TEST_F(StoredLibraryTest, OneMissingFileMakesTheWholeLoadAMiss) {
    const LibrarySource first = source("a.sv", "module a; endmodule\n");
    const LibrarySource second = source("b.sv", "module b; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&first, 1}, sources, designWith("a", first.buffer), engine));

    const std::vector<LibrarySource> both{first, second};
    Design loaded;
    EXPECT_FALSE(library.load(both, sources, loaded));
    EXPECT_TRUE(loaded.empty()) << "a partial hit must leave the design untouched";
}

TEST_F(StoredLibraryTest, SeveralFilesStoreAndLoadTogether) {
    const LibrarySource first = source("a.sv", "module a; endmodule\n");
    const LibrarySource second = source("b.sv", "module b; endmodule\n");
    const std::vector<LibrarySource> both{first, second};

    Design design = designWith("a", first.buffer);
    ModuleDefinition other;
    other.name = "b";
    other.location = SourceLocation(second.buffer, 0);
    design.addModule(std::move(other));

    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store(both, sources, design, engine));

    Design loaded;
    ASSERT_TRUE(library.load(both, sources, loaded));
    EXPECT_EQ(library.entriesLoaded(), 2U);
    ASSERT_EQ(loaded.size(), 2U);
    EXPECT_NE(loaded.findModule("a"), nullptr);
    EXPECT_NE(loaded.findModule("b"), nullptr);
}

TEST_F(StoredLibraryTest, AFileThatDefinesNothingStillGetsAnEntry) {
    // A header of macros defines no unit. Without an entry it would look like a miss for ever,
    // and a design that includes one would never reuse anything.
    const LibrarySource header = source("defs.svh", "`define WIDTH 8\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&header, 1}, sources, Design(), engine));

    Design loaded;
    EXPECT_TRUE(library.load({&header, 1}, sources, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST_F(StoredLibraryTest, StoringAgainReplacesTheEntry) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("renamed", file.buffer), engine));

    Design loaded;
    ASSERT_TRUE(library.load({&file, 1}, sources, loaded));
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded.module(0).name, "renamed");
}

// ---------------------------------------------------------------------------
// Refusing what cannot be trusted
// ---------------------------------------------------------------------------

TEST_F(StoredLibraryTest, ACorruptEntryIsAMissRatherThanACrash) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    // Section 4.4 requires a mismatch to cause clean re-analysis rather than a crash or,
    // worse, silent misbehaviour.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(library.path())) {
        std::ofstream corrupted(entry.path(), std::ios::binary | std::ios::trunc);
        corrupted << "ENZANEIR" << std::string(40, '\xA5');
    }

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST_F(StoredLibraryTest, AnEmptyEntryIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(library.path())) {
        const std::ofstream truncated(entry.path(), std::ios::binary | std::ios::trunc);
    }

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AFailedWriteIsReportedAndDoesNotFailTheRun) {
    // The same reason SourceManagerFileTest skips its unreadable-file case: a directory mode
    // does not restrict root, so the write this expects to fail would succeed.
    if (::geteuid() == 0) GTEST_SKIP() << "directory permissions do not restrict root";

    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);

    // A directory where the entry wants to be: the write fails, and the caller still has the
    // design it analysed.
    std::filesystem::permissions(library.path(), std::filesystem::perms::owner_read
                                                     | std::filesystem::perms::owner_exec);

    EXPECT_FALSE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    EXPECT_TRUE(reported(DiagCode::LibraryWriteFailed));

    std::filesystem::permissions(library.path(), std::filesystem::perms::owner_all);
}

// ---------------------------------------------------------------------------
// A library read rather than written
// ---------------------------------------------------------------------------

TEST_F(LibraryTest, EveryUnitIsLoadedAndStampedWithTheLibrarysName) {
    const std::filesystem::path path = workspace / "acme";
    const LibrarySource file = source("a.sv", "module a; endmodule\n");

    {
        Library writing;
        ASSERT_TRUE(writing.create(path, {}, engine));
        ASSERT_TRUE(writing.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    }

    Library reading;
    ASSERT_TRUE(reading.open(path, engine));

    Design design;
    ASSERT_TRUE(reading.loadAll(design, engine));
    ASSERT_NE(design.findModule("a"), nullptr);
    EXPECT_EQ(design.findModule("a")->library, "acme");
    EXPECT_NE(design.findModule("acme.a"), nullptr) << "and is reachable qualified";
}

TEST_F(LibraryTest, AnOverriddenNameIsWhatTheUnitsCarry) {
    const std::filesystem::path path = workspace / "work";
    const LibrarySource file = source("a.sv", "module a; endmodule\n");

    {
        Library writing;
        ASSERT_TRUE(writing.create(path, {}, engine));
        ASSERT_TRUE(writing.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    }

    Library reading;
    ASSERT_TRUE(reading.open(path, engine));
    ASSERT_TRUE(reading.overrideName("vendor"));

    Design design;
    ASSERT_TRUE(reading.loadAll(design, engine));
    EXPECT_NE(design.findModule("vendor.a"), nullptr);
}

TEST_F(LibraryTest, AnOverrideThatIsNotAnIdentifierIsRefused) {
    const std::filesystem::path path = workspace / "work";
    Library library;
    ASSERT_TRUE(library.create(path, {}, engine));

    EXPECT_FALSE(library.overrideName("not.a.name"));
    EXPECT_EQ(library.name(), "work") << "and the library keeps the name it had";
}

TEST_F(LibraryTest, ADamagedEntryFailsTheLoadRatherThanBeingSkipped) {
    // A library read in part is a design missing modules it will never mention, which is the
    // failure mode section 1.2 ranks worst.
    const std::filesystem::path path = workspace / "work";
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    {
        Library writing;
        ASSERT_TRUE(writing.create(path, {}, engine));
        ASSERT_TRUE(writing.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    }

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() != ".eirlib") continue;
        std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << "rubbish";
    }

    Library reading;
    ASSERT_TRUE(reading.open(path, engine));

    Design design;
    EXPECT_FALSE(reading.loadAll(design, engine));
    EXPECT_TRUE(reported(DiagCode::LibraryEntryUnreadable));
}

TEST_F(LibraryTest, ListingNamesTheUnitsItHolds) {
    const std::filesystem::path path = workspace / "work";
    const LibrarySource file = source("a.sv", "module a; endmodule\n");

    Library library;
    ASSERT_TRUE(library.create(path, {}, engine));
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    EXPECT_EQ(library.unitNames(), std::vector<std::string>{"a"});
}

TEST_F(StoredLibraryTest, AnEntryIsNamedAfterItsSourceAndStaysThereWhenItChanges) {
    // The name must not move when the file is edited, or every save leaves the previous entry
    // behind and the library grows without bound.
    const LibrarySource first = source("counter.sv", "module counter; endmodule\n");
    ASSERT_TRUE(library.store({&first, 1}, sources, designWith("counter", first.buffer), engine));

    std::vector<std::filesystem::path> before;
    for (const auto& e : std::filesystem::directory_iterator(library.path()))
        if (e.path().extension() == ".eirlib") before.push_back(e.path().filename());

    ASSERT_EQ(before.size(), 1U);
    EXPECT_NE(before.front().string().find("counter.sv"), std::string::npos)
        << "an entry is readable as the source it came from: " << before.front();

    const LibrarySource changed = source("counter.sv", "module counter; wire w; endmodule\n");
    ASSERT_TRUE(
        library.store({&changed, 1}, sources, designWith("counter", changed.buffer), engine));

    std::vector<std::filesystem::path> after;
    for (const auto& e : std::filesystem::directory_iterator(library.path()))
        if (e.path().extension() == ".eirlib") after.push_back(e.path().filename());

    EXPECT_EQ(after, before) << "the entry was rewritten in place, not written beside the old one";
}

// ---------------------------------------------------------------------------
// VHDL (sections 4.4 and 8.2)
// ---------------------------------------------------------------------------

/// The options a VHDL source is analysed with, as the session puts them in its key.
constexpr std::string_view VhdlOptions = "std 2008\nwork work";

/// Makes `library`'s VHDL directory, as NVC's analysis does when it writes the units.
void analysedByNvc(const Library& library) {
    std::filesystem::create_directories(library.vhdlDirectory());
}

TEST_F(StoredLibraryTest, AVhdlSourceStoredIsCurrentAndDeclaresItsUnits) {
    const LibrarySource file = source("counter.vhd", "entity counter is end entity;\n");
    analysedByNvc(library);
    const std::vector<std::vector<std::string>> units{{"counter"}};
    ASSERT_TRUE(library.storeVhdl({&file, 1}, sources, VhdlOptions, units, engine));

    EXPECT_TRUE(library.holdsVhdl());
    EXPECT_TRUE(library.vhdlCurrent({&file, 1}, sources, VhdlOptions));
    EXPECT_EQ(library.vhdlUnits(), std::vector<std::string>{"counter"});
    EXPECT_EQ(library.unitNames(), std::vector<std::string>{"counter"}) << "`lib list` shows it";
}

TEST_F(StoredLibraryTest, AVhdlSourceEditedOrAnalysedOtherwiseIsStale) {
    const LibrarySource file = source("counter.vhd", "entity counter is end entity;\n");
    analysedByNvc(library);
    const std::vector<std::vector<std::string>> units{{"counter"}};
    ASSERT_TRUE(library.storeVhdl({&file, 1}, sources, VhdlOptions, units, engine));

    const LibrarySource edited = source("counter.vhd", "entity counter is port (a : bit); end;\n");
    EXPECT_FALSE(library.vhdlCurrent({&edited, 1}, sources, VhdlOptions));
    EXPECT_FALSE(library.vhdlCurrent({&file, 1}, sources, "std 1993\nwork work"));
}

TEST_F(StoredLibraryTest, VhdlEntriesWithoutNvcsUnitsAreStale) {
    const LibrarySource file = source("counter.vhd", "entity counter is end entity;\n");
    const std::vector<std::vector<std::string>> units{{"counter"}};
    ASSERT_TRUE(library.storeVhdl({&file, 1}, sources, VhdlOptions, units, engine));
    EXPECT_FALSE(library.vhdlCurrent({&file, 1}, sources, VhdlOptions))
        << "an entry says what NVC analysed; without NVC's units it describes nothing";
}

TEST_F(StoredLibraryTest, VhdlHoldsNoTimeUnit) {
    // A VHDL design is translated in the run's unit when it runs, so a library of VHDL holds no
    // delays and fixes no unit: it may be read beside one of SystemVerilog in any.
    const LibrarySource file = source("counter.vhd", "entity counter is end entity;\n");
    analysedByNvc(library);
    library.setOptions({}, {}, {}, -15);
    const std::vector<std::vector<std::string>> units{{"counter"}};
    ASSERT_TRUE(library.storeVhdl({&file, 1}, sources, VhdlOptions, units, engine));
    EXPECT_FALSE(library.timeUnit().has_value());

    Design loaded;
    ASSERT_TRUE(library.loadAll(loaded, engine));
    EXPECT_EQ(loaded.size(), 0U) << "a VHDL entry holds no units of EIR";
}

TEST_F(StoredLibraryTest, TheVhdlStampChangesWhenItsVhdlIsAnalysedAgain) {
    EXPECT_TRUE(library.vhdlStamp().empty());

    const LibrarySource file = source("pkg.vhd", "package pkg is end package;\n");
    analysedByNvc(library);
    const std::vector<std::vector<std::string>> units{{"pkg"}};
    ASSERT_TRUE(library.storeVhdl({&file, 1}, sources, VhdlOptions, units, engine));
    const std::string first = library.vhdlStamp();
    EXPECT_FALSE(first.empty());

    const LibrarySource edited = source("pkg.vhd", "package pkg is constant c : bit; end;\n");
    ASSERT_TRUE(library.storeVhdl({&edited, 1}, sources, VhdlOptions, units, engine));
    EXPECT_NE(library.vhdlStamp(), first);
}

TEST_F(StoredLibraryTest, CleaningRemovesEveryEntryAndNvcsUnits) {
    const LibrarySource sv = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {}, {}, Picoseconds);
    ASSERT_TRUE(library.store({&sv, 1}, sources, designWith("a", sv.buffer), engine));
    const LibrarySource vhdl = source("counter.vhd", "entity counter is end entity;\n");
    analysedByNvc(library);
    const std::vector<std::vector<std::string>> units{{"counter"}};
    ASSERT_TRUE(library.storeVhdl({&vhdl, 1}, sources, VhdlOptions, units, engine));

    EXPECT_EQ(library.clean(), 2U);
    EXPECT_FALSE(library.holdsVhdl());
    EXPECT_FALSE(std::filesystem::exists(library.vhdlDirectory()));
    EXPECT_TRUE(library.unitNames().empty());
    EXPECT_EQ(library.name(), "work") << "the library and its name stay";
}

} // namespace
