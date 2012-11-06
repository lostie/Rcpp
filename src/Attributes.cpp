// -*- mode: C++; c-indent-level: 4; c-basic-offset: 4; indent-tabs-mode: nil; -*-
//
// Attributes.cpp: Rcpp R/C++ interface class library -- Rcpp attributes
//
// Copyright (C) 2012 JJ Allaire, Dirk Eddelbuettel and Romain Francois
//
// This file is part of Rcpp.
//
// Rcpp is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Rcpp is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Rcpp.  If not, see <http://www.gnu.org/licenses/>.


#include <fstream>
#include <cstring>
#include <map>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <Rcpp.h>
#include <Rcpp/exceptions.h>

#include "AttributesParser.h"

using namespace Rcpp::attributes_parser;

namespace {

    // Utility class for getting file existence and last modified time
    class FileInfo {
    public:    
        explicit FileInfo(const std::string& path) 
            : path_(path), exists_(false), lastModified_(0)
        {
    #ifdef _WIN32
            struct _stat buffer;
            int result = _stat(path.c_str(), &buffer);
    #else
            struct stat buffer;
            int result = stat(path.c_str(), &buffer);
    #endif  
            if (result != 0) {
                if (errno == ENOENT)
                    exists_ = false;
                else
                    throw Rcpp::file_io_error(errno, path);
            } else {
                exists_ = true;
                lastModified_ = buffer.st_mtime;
            }
        }
        
        std::string path() const { return path_; }
        bool exists() const { return exists_; }
        time_t lastModified() const { return lastModified_; }
        
    private:
        std::string path_;
        bool exists_;
        time_t lastModified_;
    };
    
    // Remove a file (call back into R for this)
    void removeFile(const std::string& path) {
        if (FileInfo(path).exists()) {
            Rcpp::Function rm = Rcpp::Environment::base_env()["file.remove"];
            rm(path);
        }
    }
    
    // Recursively create a directory (call back into R for this)
    void createDirectory(const std::string& path) {
        if (!FileInfo(path).exists()) {
            Rcpp::Function mkdir = Rcpp::Environment::base_env()["dir.create"];
            mkdir(path, Rcpp::Named("recursive") = true);
        }
    }
    
    
    // Check if the passed attribute represents an exported function
    bool isExportedFunction(const Attribute& attribute) {
        return (attribute.name() == kExportAttribute) &&
               !attribute.function().empty();
    }
    
    // Determine the exported name for a function 
    std::string exportedName(const Attribute& attribute) {   
        if (!attribute.params().empty())
            return attribute.params()[0].name();
        else
            return attribute.function().name();
    }
    
    // Generate function entries for passed attributes
    void generateCppModuleFunctions(std::ostream& ostr,
                                    const SourceFileAttributes& attributes,
                                    bool verbose)
    {
        for(std::vector<Attribute>::const_iterator 
                it = attributes.begin(); it != attributes.end(); ++it) {
            
            // verify this is an exported function 
            if (isExportedFunction(*it)) {
                     
                // verbose output
                const Function& function = it->function();
                if (verbose)
                    Rcpp::Rcout << "  " << function << std::endl;
              
                // add module function export
                ostr << "    Rcpp::function(\"" << exportedName(*it) << "\", &"
                     << function.name() << ");" << std::endl;
                      
            } 
        }
    }
    
    // Generate a module declaration
   void generateCppModule(std::ostream& ostr,
                          const std::string& moduleName, 
                          const SourceFileAttributes& attributes,
                          bool verbose) {    
        ostr << "RCPP_MODULE(" << moduleName  << ") {" << std::endl;
        generateCppModuleFunctions(ostr, attributes, verbose);
        ostr << "}" << std::endl;
    }    
    
    // Generate placeholder function declaration (for roxygen)
    void generateRoxygenPlaceholder(std::ostream& ostr,
                                    const Attribute& attribute) {
        
        ostr << exportedName(attribute) << "<- function(";
        const std::vector<Argument>& args = attribute.function().arguments();
        for (std::size_t i=0; i<args.size(); i++) {
            ostr << args[i].name();
            if (i != (args.size()-1))
                ostr << ", ";
        }
        ostr << ") {}" << std::endl;
    }
    
    // Generate roxygen
    void generateRoxygen(std::ostream& ostr,
                         const SourceFileAttributes& attributes) {
        
        for(std::vector<Attribute>::const_iterator 
                it = attributes.begin(); it != attributes.end(); ++it) {
         
            if (isExportedFunction(*it) && !it->roxygen().empty()) {
                ostr << std::endl;
                for (std::size_t i=0; i<it->roxygen().size(); i++)
                    ostr << it->roxygen()[i] << std::endl;
                generateRoxygenPlaceholder(ostr, *it);
                ostr << std::endl;
            } 
        }
    }
    
    // Class that manages generation of source code for the sourceCpp dynlib
    class SourceCppDynlib {
    public:
        SourceCppDynlib() {}
        
        SourceCppDynlib(const std::string& cppSourcePath, Rcpp::List platform) 
            :  cppSourcePath_(cppSourcePath)
               
        {
            // get cpp source file info 
            FileInfo cppSourceFilenameInfo(cppSourcePath_);
            if (!cppSourceFilenameInfo.exists())
                throw Rcpp::file_not_found(cppSourcePath_);
                    
            // get last modified time        
            cppSourceLastModified_ = cppSourceFilenameInfo.lastModified();
            
            // record the base name of the source file
            Rcpp::Function basename = Rcpp::Environment::base_env()["basename"];
            cppSourceFilename_ = Rcpp::as<std::string>(basename(cppSourcePath_));
            
            // get platform info
            fileSep_ = Rcpp::as<std::string>(platform["file.sep"]);
            dynlibExt_ = Rcpp::as<std::string>(platform["dynlib.ext"]);
            
            // generate temp directory 
            Rcpp::Function tempfile = Rcpp::Environment::base_env()["tempfile"];
            buildDirectory_ = Rcpp::as<std::string>(tempfile("sourcecpp_"));
            std::replace(buildDirectory_.begin(), buildDirectory_.end(), '\\', '/');
            Rcpp::Function dircreate = Rcpp::Environment::base_env()["dir.create"];
            dircreate(buildDirectory_);
            
            // generate a random module name
            Rcpp::Function sample = Rcpp::Environment::base_env()["sample"];
            std::ostringstream ostr;
            ostr << "sourceCpp_" << Rcpp::as<int>(sample(100000, 1));
            moduleName_ = ostr.str();
            
            // regenerate the source code
            regenerateSource();
        }
        
        bool isEmpty() const { return cppSourcePath_.empty(); }
        
        bool isBuilt() const { return FileInfo(dynlibPath()).exists(); };
                
        bool isSourceDirty() const {          
            // source file out of date means we're dirty
            if (FileInfo(cppSourcePath_).lastModified() > 
                FileInfo(generatedCppSourcePath()).lastModified())
                return true;
                     
            // no dynlib means we're dirty
            if (!FileInfo(dynlibPath()).exists())
                return true;
                
            // not dirty
            return false;
        }
        
        void regenerateSource() {
            
            // copy the source file to the build dir
            Rcpp::Function filecopy = Rcpp::Environment::base_env()["file.copy"];
            filecopy(cppSourcePath_, generatedCppSourcePath(), true);
            
            // parse attributes
            SourceFileAttributes sourceAttributes(cppSourcePath_);
        
            // generate RCPP module
            std::ostringstream ostr;
            generateCppModule(ostr, moduleName(), sourceAttributes, false); 
            generatedCpp_ = ostr.str();
            
            // open source file and append module
            std::ofstream cppOfs(generatedCppSourcePath().c_str(), 
                                 std::ofstream::out | std::ofstream::app);
            if (cppOfs.fail())
                throw Rcpp::file_io_error(generatedCppSourcePath());
            cppOfs << std::endl;
            cppOfs << generatedCpp_;
            cppOfs.close();
               
            // discover exported functions, and dependencies
            exportedFunctions_.clear();
            depends_.clear();
            for (SourceFileAttributes::const_iterator 
              it = sourceAttributes.begin(); it != sourceAttributes.end(); ++it) {
                 if (it->name() == kExportAttribute && !it->function().empty()) 
                    exportedFunctions_.push_back(exportedName(*it));
                
                 else if (it->name() == kDependsAttribute) {
                     for (size_t i = 0; i<it->params().size(); ++i)
                        depends_.push_back(it->params()[i].name());
                 }   
            }
        }
        
        const std::string& moduleName() const {
            return moduleName_;
        }
        
        const std::string& cppSourcePath() const {
            return cppSourcePath_;
        }
        
        std::string buildDirectory() const {
            return buildDirectory_;
        }
        
        std::string generatedCpp() const {
            return generatedCpp_;
        }
        
        std::string cppSourceFilename() const {
            return cppSourceFilename_;
        }
         
        std::string dynlibFilename() const {
            return moduleName() + dynlibExt_;
        }
        
        std::string dynlibPath() const {
            return buildDirectory_ + fileSep_ + dynlibFilename();
        }
       
        const std::vector<std::string>& exportedFunctions() const {
            return exportedFunctions_;
        }
        
        const std::vector<std::string>& depends() const { return depends_; };
          
    private:
    
        std::string generatedCppSourcePath() const {
           return buildDirectory_ + fileSep_ + cppSourceFilename(); 
        }
        
    private:
        std::string cppSourcePath_;
        time_t cppSourceLastModified_;
        std::string generatedCpp_;
        std::string cppSourceFilename_;
        std::string moduleName_;
        std::string buildDirectory_;
        std::string fileSep_;
        std::string dynlibExt_;
        std::vector<std::string> exportedFunctions_;
        std::vector<std::string> depends_;
    };
    
    // Dynlib cache that allows lookup by either file path or code contents
    class SourceCppDynlibCache {
      
    public:
        SourceCppDynlibCache() {}
        
    private:
        // prohibit copying
        SourceCppDynlibCache(const SourceCppDynlibCache&);
        SourceCppDynlibCache& operator=(const SourceCppDynlibCache&); 
      
    public:
        // Insert into cache by file name
        void insertFile(const std::string& file, const SourceCppDynlib& dynlib) {
            Entry entry;
            entry.file = file;
            entry.dynlib = dynlib;
            entries_.push_back(entry);
        }
        
        // Insert into cache by code
        void insertCode(const std::string& code, const SourceCppDynlib& dynlib) {
            Entry entry;
            entry.code = code;
            entry.dynlib = dynlib;
            entries_.push_back(entry);
        }
    
        // Lookup by file
        SourceCppDynlib lookupByFile(const std::string& file) {
            for (std::size_t i = 0; i < entries_.size(); i++) {
                if (entries_[i].file == file)
                    return entries_[i].dynlib;
            }
            
            return SourceCppDynlib();
        }
        
        // Lookup by code
        SourceCppDynlib lookupByCode(const std::string& code) {
            for (std::size_t i = 0; i < entries_.size(); i++) {
                if (entries_[i].code == code)
                    return entries_[i].dynlib;
            }
            
            return SourceCppDynlib();
        }
      
    private:
        struct Entry {
            std::string file;
            std::string code;
            SourceCppDynlib dynlib;
        };
        std::vector<Entry> entries_;
    };
    
    // Abstract class which manages writing of code for compileAttributes
    class ExportsGenerator {
    protected:
        ExportsGenerator(const std::string& targetFile, 
                         const std::string& commentPrefix) 
            : targetFile_(targetFile), commentPrefix_(commentPrefix) {
            
            // read the existing target file if it exists
            if (FileInfo(targetFile_).exists()) {
                std::ifstream ifs(targetFile_.c_str());
                if (ifs.fail())
                    throw Rcpp::file_io_error(targetFile_);
                std::stringstream buffer;
                buffer << ifs.rdbuf();
                existingCode_ = buffer.str();
            }
            
            // see if this is safe to overwite and throw if it isn't
            if (!isSafeToOverwrite())
                throw Rcpp::file_exists(targetFile_);
        }
 
    private:
        // prohibit copying
        ExportsGenerator(const ExportsGenerator&);
        ExportsGenerator& operator=(const ExportsGenerator&); 
        
    public:
        virtual ~ExportsGenerator() {}
        
        // Abstract interface for code generation
        virtual void writeBegin() = 0;
        virtual void writeFunctions(const SourceFileAttributes &attributes,
                                    bool verbose) = 0;
        virtual void writeEnd() = 0;
        
        virtual bool commit(const std::vector<std::string>& includes,
                            const std::vector<std::string>& prototypes) = 0;
        
        // Allow generator to appear as a std::ostream&
        operator std::ostream&() {
            return codeStream_;
        }
        
    protected: 
    
        // Allow access to the output stream 
        std::ostream& ostr() {
            return codeStream_;
        }
    
        // Commit the stream -- is a no-op if the existing code is identical
        // to the generated code. Returns true if data was written and false
        // if it wasn't (throws exception on io error)
        bool commit(const std::string& preamble = std::string()) {
            
            // get the generated code
            std::string code = codeStream_.str();
            
            // if there is no generated code AND the exports file does not 
            // currently exist then do nothing
            if (code.empty() && !FileInfo(targetFile_).exists())
                return false;
            
            // write header/preamble
            std::ostringstream headerStream;
            headerStream << commentPrefix_ << " This file was generated by "
                         << "Rcpp::compileAttributes" << std::endl;
            headerStream << commentPrefix_ << " Generator token: " 
                         << generatorToken() << std::endl << std::endl;      
            if (!preamble.empty())
                headerStream << preamble;
                
            // get generated code and only write it if there was a change
            std::string generatedCode = headerStream.str() + code;        
            if (generatedCode != existingCode_) {
                // open the file
                std::ofstream ofs(targetFile_.c_str(), 
                                  std::ofstream::out | std::ofstream::trunc);
                if (ofs.fail())
                    throw Rcpp::file_io_error(targetFile_);
                
                // write generated code and return
                ofs << generatedCode;
                ofs.close();
                return true;
            } 
            else {
                return false;
            }
        }
        
        // Remove the generated file entirely
        void remove() {
            removeFile(targetFile_);
        }
        
    private:
    
        // Check whether it's safe to overwrite this file (i.e. whether we 
        // generated the file in the first place)
        bool isSafeToOverwrite() const {
            return existingCode_.empty() || 
                   (existingCode_.find(generatorToken()) != std::string::npos);
        }
        
        // UUID that we write into a comment within the file (so that we can 
        // strongly identify that a file was generated by us before overwriting it)
        std::string generatorToken() const {
            return "10BE3573-1514-4C36-9D1C-5A225CD40393";
        }
    
    private:
        std::string targetFile_;
        std::string commentPrefix_;
        std::string existingCode_;
        std::ostringstream codeStream_;
    };
    
    
    // Class which manages generating RcppExports.cpp
    class CppExportsGenerator : public ExportsGenerator {
    public:
        explicit CppExportsGenerator(const std::string& packageDir, 
                                     const std::string& fileSep)
            : ExportsGenerator( 
                packageDir + fileSep + "src" +  fileSep + "RcppExports.cpp", 
                "//")
        {
        }
        
        virtual void writeBegin() {
            ostr() << "RCPP_MODULE(RcppExports) {" << std::endl;
        }
        
        virtual void writeFunctions(const SourceFileAttributes &attributes,
                                    bool verbose) {
            // verbose output if requested
            if (verbose) {
                Rcpp::Rcout << "Exports from " << attributes.sourceFile() << ":" 
                            << std::endl;
            }
        
            // generate functions
            generateCppModuleFunctions(ostr(), attributes, verbose);
         
            // verbose if requested
            if (verbose)
                Rcpp::Rcout << std::endl;                           
        }
    
        virtual void writeEnd() {
            ostr() << "}" << std::endl;   
        }
        
        virtual bool commit(const std::vector<std::string>& includes,
                            const std::vector<std::string>& prototypes) {
            
            // generate preamble 
            std::ostringstream ostr;
            if (!includes.empty()) {
                for (std::size_t i=0;i<includes.size(); i++)
                    ostr << includes[i] << std::endl;
                ostr << std::endl;
            }
            
            if (!prototypes.empty()) {
                for (std::size_t i=0;i<prototypes.size(); i++)
                    ostr << prototypes[i] << ";" << std::endl;
                ostr << std::endl;
            }
            
            // commit with preamble
            return ExportsGenerator::commit(ostr.str());
                                
        }
    
    };
    
    // Class which manages generating RcppExports.cpp
    class CppIncludeGenerator : public ExportsGenerator {
    public:
        explicit CppIncludeGenerator(const std::string& packageDir, 
                                     const std::string& fileSep,
                                     const std::string& scope)
            : ExportsGenerator( 
                packageDir +  fileSep + "inst" +  fileSep + "include" +
                fileSep + scope + ".hpp", 
                "//")
        {
            scope_ = scope;
            includeDir_ = packageDir +  fileSep + "inst" +  fileSep + "include";
            hasCppInterface_ = false; 
        }
        
        virtual void writeBegin() {
            ostr() << "namespace " << scope_ << " {" << std::endl;
        }
    
        virtual void writeFunctions(const SourceFileAttributes &attributes,
                                    bool verbose) {
                                        
            // don't write anything if there is no C++ interface
            if (!attributes.hasInterface(kInterfaceCpp)) 
                return;
            
            // there is a C++ interface so set flag indicating so
            hasCppInterface_ = true;
                                        
            for(std::vector<Attribute>::const_iterator 
                it = attributes.begin(); it != attributes.end(); ++it) {
         
                if (isExportedFunction(*it)) {
                    
                    Function function = 
                        it->function().renamedTo(exportedName(*it));
                        
                    // if the function starts with "." then it's a 
                    // a hidden R-only function
                    if (function.name().find_first_of('.') == 0)
                        continue;  
                    
                    ostr() << "    inline " << function << " {" 
                            << std::endl;
                    
                    ostr() << "        static " << function.type() 
                           << "(*p_" << function.name() << ")(";
                    
                    const std::vector<Argument>& args = 
                                                function.arguments();
                    
                    for (std::size_t i = 0; i<args.size(); i++) {
                        ostr() << args[i].type();
                        if (i != (args.size()-1))
                            ostr() << ",";
                    }
                    
                    ostr() << ") = Rcpp::GetCppCallable(\"RcppExports\", "
                           << "\"" << function.name() << "\");" 
                           << std::endl;
                    
                    ostr() << "        return p_" << function.name()
                           << "(";
                           
                    for (std::size_t i = 0; i<args.size(); i++) {
                        ostr() << args[i].name();
                        if (i != (args.size()-1))
                            ostr() << ",";
                    }
                           
                    ostr() << ");" << std::endl;
                    ostr() << "    }" << std::endl;
                    
                    
                } 
            }                           
        }
        
        virtual void writeEnd() {
            ostr() << "}" << std::endl;
        }
        
        virtual bool commit(const std::vector<std::string>& includes,
                            const std::vector<std::string>& prototypes) {
            
            if (hasCppInterface_) {
                
                // create the include dir if necessary
                createDirectory(includeDir_);
                
                // generate preamble 
                std::ostringstream ostr;
                if (!includes.empty()) {
                    for (std::size_t i=0;i<includes.size(); i++)
                        ostr << includes[i] << std::endl;
                    ostr << std::endl;
                }
                
                // commit with preamble
                return ExportsGenerator::commit(ostr.str());
            }
            else {
                ExportsGenerator::remove();
                return false;
            }
        }
        
    private:
        std::string scope_;
        std::string includeDir_;
        bool hasCppInterface_;
    };
    
    // Class which manages generator RcppExports.R
    class RExportsGenerator : public ExportsGenerator {
    public:
        explicit RExportsGenerator(const std::string& packageDir, 
                                   const std::string& fileSep)
            : ExportsGenerator(
                packageDir + fileSep + "R" +  fileSep + "RcppExports.R", 
                "#")
        {
        }
        
        virtual void writeBegin() {
        }
        
        virtual void writeFunctions(const SourceFileAttributes &attributes,
                                    bool verbose) {
                                        
            // add to exported functions if we have an R interface
            if (attributes.hasInterface(kInterfaceR)) {
                
                // track exported functions
                for (SourceFileAttributes::const_iterator 
                     it = attributes.begin(); it != attributes.end(); ++it) {
                    if (isExportedFunction(*it)) {
                        rExports_.push_back(exportedName(*it));
                    }
                }
                
                // generate roxygen 
                generateRoxygen(ostr(), attributes);      
            }                      
        }
        
        virtual void writeEnd() { 
            
            ostr() << "Rcpp::loadModule(\"RcppExports\", ";
            
            if (rExports_.size() > 0) {
                ostr() << "what = c(";
                for (size_t i=0; i<rExports_.size(); i++) {
                    if (i != 0)
                        ostr() << "                                         ";
                    ostr() << "\"" << rExports_[i] << "\"";
                    if (i != (rExports_.size()-1))
                        ostr() << "," << std::endl;
                }
                ostr() << "))" << std::endl;
            }
            else {
                ostr() << "what = character())";
            }
        }
        
        virtual bool commit(const std::vector<std::string>& includes,
                            const std::vector<std::string>& prototypes) {
            return ExportsGenerator::commit();                    
        }
    
    private:
        std::vector<std::string> rExports_;
    };
    
    // Class to manage and dispatch to a list of generators
    class ExportsGenerators {
    public:
        typedef std::vector<ExportsGenerator*>::iterator Itr;
        
        ExportsGenerators() {}
        
        virtual ~ExportsGenerators() {
            try {
                for(Itr it = generators_.begin(); it != generators_.end(); ++it)
                    delete *it;
                generators_.clear(); 
            }
            catch(...) {}
        }
        
        void add(ExportsGenerator* pGenerator) {
            generators_.push_back(pGenerator);
        }
        
        void writeBegin() {
            for(Itr it = generators_.begin(); it != generators_.end(); ++it)
                (*it)->writeBegin();
        }
        
        void writeFunctions(const SourceFileAttributes &attributes,
                            bool verbose) {
            for(Itr it = generators_.begin(); it != generators_.end(); ++it)
                (*it)->writeFunctions(attributes, verbose);
        }
        
        void writeEnd() {
            for(Itr it = generators_.begin(); it != generators_.end(); ++it)
                (*it)->writeEnd();
        }
        
        bool commit(const std::vector<std::string>& includes,
                    const std::vector<std::string>& prototypes) {
            
            bool wrote = false;
            
            for(Itr it = generators_.begin(); it != generators_.end(); ++it) {
               if ((*it)->commit(includes, prototypes))
                wrote = true;
            }
               
            return wrote;
        }
    
    private:
        // prohibit copying
        ExportsGenerators(const ExportsGenerators&);
        ExportsGenerators& operator=(const ExportsGenerators&); 
        
    private:
        std::vector<ExportsGenerator*> generators_;
    };

} // anonymous namespace


// Create temporary build directory, generate code as necessary, and return
// the context required for the sourceCpp function to complete it's work
RcppExport SEXP sourceCppContext(SEXP sFile, SEXP sCode, SEXP sPlatform) {
BEGIN_RCPP
    // parameters
    std::string file = Rcpp::as<std::string>(sFile);
    std::string code = sCode != R_NilValue ? Rcpp::as<std::string>(sCode) : "";
    Rcpp::List platform = Rcpp::as<Rcpp::List>(sPlatform);
    
    // get dynlib (using cache if possible)
    static SourceCppDynlibCache s_dynlibCache;
    SourceCppDynlib dynlib;
    if (!code.empty())
        dynlib = s_dynlibCache.lookupByCode(code);
    else
        dynlib = s_dynlibCache.lookupByFile(file);
  
    // check dynlib build state
    bool buildRequired = false;
    
    // if there is no dynlib in the cache then create one
    if (dynlib.isEmpty()) {   
        buildRequired = true;
        dynlib = SourceCppDynlib(file, platform);
        if (!code.empty())
            s_dynlibCache.insertCode(code, dynlib);
        else
            s_dynlibCache.insertFile(file, dynlib);
    }    
        
    // if the cached dynlib is dirty then regenerate the source
    else if (dynlib.isSourceDirty()) {
        buildRequired = true;    
        dynlib.regenerateSource();
    }
    
    // if the dynlib hasn't yet been built then note that
    else if (!dynlib.isBuilt()) {
        buildRequired = true; 
    }
    
    // return context as a list
    Rcpp::List context;
    context["moduleName"] = dynlib.moduleName();
    context["cppSourcePath"] = dynlib.cppSourcePath();
    context["buildRequired"] = buildRequired;
    context["buildDirectory"] = dynlib.buildDirectory();
    context["generatedCpp"] = dynlib.generatedCpp();
    context["exportedFunctions"] = dynlib.exportedFunctions();
    context["cppSourceFilename"] = dynlib.cppSourceFilename();
    context["dynlibFilename"] = dynlib.dynlibFilename();
    context["dynlibPath"] = dynlib.dynlibPath();
    context["depends"] = dynlib.depends();
    return Rcpp::wrap(context);
END_RCPP
}

// Compile the attributes within the specified package directory into 
// RcppExports.cpp and RcppExports.R
RcppExport SEXP compileAttributes(SEXP sPackageDir, 
                                  SEXP sPackageName,
                                  SEXP sCppFiles,
                                  SEXP sCppFileBasenames,
                                  SEXP sIncludes,
                                  SEXP sVerbose,
                                  SEXP sPlatform) {
BEGIN_RCPP
    // arguments
    std::string packageDir = Rcpp::as<std::string>(sPackageDir);
    std::string packageName = Rcpp::as<std::string>(sPackageName);
    std::vector<std::string> cppFiles = 
                    Rcpp::as<std::vector<std::string> >(sCppFiles);
    std::vector<std::string> cppFileBasenames = 
                    Rcpp::as<std::vector<std::string> >(sCppFileBasenames);
    std::vector<std::string> includes = 
                    Rcpp::as<std::vector<std::string> >(sIncludes);
    bool verbose = Rcpp::as<bool>(sVerbose);
    Rcpp::List platform = Rcpp::as<Rcpp::List>(sPlatform);
    std::string fileSep = Rcpp::as<std::string>(platform["file.sep"]); 
     
    // initialize generators and namespace/prototype vectors
    ExportsGenerators generators;
    generators.add(new CppExportsGenerator(packageDir, fileSep));
    generators.add(new RExportsGenerator(packageDir, fileSep));
    generators.add(new CppIncludeGenerator(packageDir, fileSep, packageName));
    std::vector<std::string> prototypes;
    
    // write begin
    generators.writeBegin();
     
    // Parse attributes from each file and generate code as required. 
    for (std::size_t i=0; i<cppFiles.size(); i++) {
        
        // parse attributes (continue if there are none)
        std::string cppFile = cppFiles[i];
        SourceFileAttributes attributes(cppFile);
        if (attributes.empty())
            continue;
            
        // copy prototypes
        std::copy(attributes.prototypes().begin(),
                  attributes.prototypes().end(),
                  std::back_inserter(prototypes));
        
        // write functions
        generators.writeFunctions(attributes, verbose);
    }
    
    // write end
    generators.writeEnd();

    // commit 
    bool wrote = generators.commit(includes, prototypes);  
                                                                                                                   
    // verbose output
    if (verbose) {
        if (wrote)
            Rcpp::Rcout << "Rcpp exports files updated" << std::endl;
        else
            Rcpp::Rcout << "Rcpp exports files already up to date" << std::endl;
    }
    
    // return status
    return Rcpp::wrap<bool>(wrote);
END_RCPP
}