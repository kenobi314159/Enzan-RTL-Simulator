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

TEST_F(LibraryTest, OpeningCreatesTheDirectory) {
    Library library;
    const std::filesystem::path path = workspace / "work";

    ASSERT_TRUE(library.open(path, engine));
    EXPECT_TRUE(library.isOpen());
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_EQ(library.path(), path);
}

TEST_F(LibraryTest, OpeningAnExistingDirectoryIsFine) {
    const std::filesystem::path path = workspace / "work";
    std::filesystem::create_directories(path);

    Library library;
    EXPECT_TRUE(library.open(path, engine));
}

TEST_F(LibraryTest, RefusesAPathThatIsAFile) {
    workspace.write("work", "not a directory");

    Library library;
    EXPECT_FALSE(library.open(workspace / "work", engine));
    EXPECT_FALSE(library.isOpen());
    EXPECT_TRUE(reported(DiagCode::LibraryNotADirectory));
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

class StoredLibraryTest : public LibraryTest {
protected:
    StoredLibraryTest() { EXPECT_TRUE(library.open(workspace / "work", engine)); }

    Library library;
};

TEST_F(StoredLibraryTest, WhatWasStoredComesBack) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});
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
    library.setOptions({}, {});
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    Design loaded;
    ASSERT_TRUE(library.load({&file, 1}, sources, loaded));
    EXPECT_EQ(loaded.module(0).location, SourceLocation(file.buffer, 0));
}

TEST_F(StoredLibraryTest, NothingStoredIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST_F(StoredLibraryTest, LoadingNothingIsAMissRatherThanAnEmptyHit) {
    library.setOptions({}, {});

    Design loaded;
    EXPECT_FALSE(library.load({}, sources, loaded));
}

TEST_F(StoredLibraryTest, ChangedTextIsAMiss) {
    const LibrarySource before = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});
    ASSERT_TRUE(library.store({&before, 1}, sources, designWith("a", before.buffer), engine));

    const LibrarySource after = source("a.sv", "module a; wire w; endmodule\n");
    Design loaded;
    EXPECT_FALSE(library.load({&after, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, TheSameTextUnderAnotherNameIsAMiss) {
    // Two files with identical contents are still two units, and a diagnostic about one must
    // name the right file.
    const LibrarySource first = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});
    ASSERT_TRUE(library.store({&first, 1}, sources, designWith("a", first.buffer), engine));

    const LibrarySource second = source("b.sv", "module a; endmodule\n");
    Design loaded;
    EXPECT_FALSE(library.load({&second, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AChangedDefineIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::string> before{"WIDTH=8"};
    library.setOptions(before, {});
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::string> after{"WIDTH=16"};
    library.setOptions(after, {});

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AChangedIncludePathIsAMiss) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::filesystem::path> before{"include"};
    library.setOptions({}, before);
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::filesystem::path> after{"include", "vendor"};
    library.setOptions({}, after);

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, TwoOptionsThatConcatenateAlikeAreDistinguished) {
    // `+define+AB` and `+define+A +define+B` must not hash to the same key, which is what a
    // digest with no separator between fields would do.
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    const std::vector<std::string> joined{"AB"};
    library.setOptions(joined, {});
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    const std::vector<std::string> split{"A", "B"};
    library.setOptions(split, {});

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, OneMissingFileMakesTheWholeLoadAMiss) {
    const LibrarySource first = source("a.sv", "module a; endmodule\n");
    const LibrarySource second = source("b.sv", "module b; endmodule\n");
    library.setOptions({}, {});
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

    library.setOptions({}, {});
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
    library.setOptions({}, {});
    ASSERT_TRUE(library.store({&header, 1}, sources, Design(), engine));

    Design loaded;
    EXPECT_TRUE(library.load({&header, 1}, sources, loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST_F(StoredLibraryTest, StoringAgainReplacesTheEntry) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});
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
    library.setOptions({}, {});
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
    library.setOptions({}, {});
    ASSERT_TRUE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(library.path())) {
        const std::ofstream truncated(entry.path(), std::ios::binary | std::ios::trunc);
    }

    Design loaded;
    EXPECT_FALSE(library.load({&file, 1}, sources, loaded));
}

TEST_F(StoredLibraryTest, AFailedWriteIsReportedAndDoesNotFailTheRun) {
    const LibrarySource file = source("a.sv", "module a; endmodule\n");
    library.setOptions({}, {});

    // A directory where the entry wants to be: the write fails, and the caller still has the
    // design it analysed.
    std::filesystem::permissions(library.path(), std::filesystem::perms::owner_read
                                                     | std::filesystem::perms::owner_exec);

    EXPECT_FALSE(library.store({&file, 1}, sources, designWith("a", file.buffer), engine));
    EXPECT_TRUE(reported(DiagCode::LibraryWriteFailed));

    std::filesystem::permissions(library.path(), std::filesystem::perms::owner_all);
}

} // namespace
