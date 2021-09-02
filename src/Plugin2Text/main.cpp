#define _CRT_SECURE_NO_WARNINGS
#include "esp_to_text.hpp"
#include "text_to_esp.hpp"
#include "common.hpp"
#include "esp_parser.hpp"
#include <stdio.h>
#include "os.hpp"
#include <stdarg.h>
#include "array.hpp"

static void print_usage(const char* hint) {
    puts(hint);
    puts(
        "Usage: plugin2text.exe <source file> [destination file]\n"
        "\n"
        "    <source file>              file to convert (*.esp, *.esm, *.esl, *.txt)\n"
        "    [destination file]         output path\n"
        "\n"
        "Options:\n"
        "\n"
        "    --time                     output elapsed time in stdout\n"
        "\n"
        "Text serialization options:\n"
        "\n"
        "    --export-timestamp         write timestamps for records\n"
        "    --preserve-record-order    always write records in the same order as in ESP\n"
        "    --preserve-junk            do not clear fields that may contain junk data\n"
        "\n"
        "If <source file> has ESP/ESM/ESL file extension, then <source file> will be converted\n"
        "to text format. If <source file> has TXT extension, then <source file> will be converted\n"
        "to TES plugin.\n"
        "\n"
        "If [destination file] is omitted, then [destination file] is <source file> with extension\n"
        "changed to plugin or text format.\n"
        "\n"
        "Examples:\n"
        "    plugin2text.exe Skyrim.esm Skyrim.txt\n"
        "        convert Skyrim.esm to text format and write resulting file to Skyrim.txt\n"
        "\n"
        "    plugin2text.exe Dawnguard.txt Dawnguard.esm\n"
        "        convert Dawnguard.txt to TES plugin and write resulting file to Dawnguard.esm\n"
    );
}

static const wchar_t* get_file_extension(const wchar_t* string) {
    int index = string_last_index_of(string, '.');
    return index ? &string[index] : L"";
}

static const wchar_t* replace_destination_file_extension(const wchar_t* source_file, const wchar_t* source_file_extension) {
    const wchar_t* destination_file_extension = L".txt";
    if (string_equals(source_file_extension, L".txt")) {
        destination_file_extension = L".esp";
    }

    const auto no_extension_count = wcslen(source_file) - wcslen(source_file_extension);
    const auto count = no_extension_count + wcslen(destination_file_extension);

    auto destination_file = new wchar_t[count + 1];
    memcpy(&destination_file[0], source_file, sizeof(wchar_t) * no_extension_count);
    memcpy(&destination_file[no_extension_count], destination_file_extension, sizeof(wchar_t) * wcslen(destination_file_extension));
    destination_file[count] = L'\0';
    return destination_file;
}

struct ArgParser {
    struct Option {
        const wchar_t* key = L"";
        const wchar_t* value = L"";
    };

    Array<wchar_t*> arguments;
    Array<wchar_t*> flags;
    Array<Option> options;

    void parse() {
        int argc = 0;
        const auto argv = get_command_line_args(&argc);

        for (int i = 1; i < argc; ++i) {
            auto arg = argv[i];
            if (string_starts_with(arg, L"--")) {
                auto option_start = arg + 2;
                auto assign_index = string_index_of(option_start, '=');
                if (assign_index != -1) {
                    Option option;
                    option.key = substring(option_start, option_start + assign_index);
                    option.value = substring(option_start + assign_index + 1, option_start + assign_index + wcslen(option_start));
                    options.push(option);
                } else {
                    flags.push(option_start);
                }
            } else {
                arguments.push(arg);
            }
        }
    }
};

struct Args {
    const wchar_t* source_file = nullptr;
    const wchar_t* destination_file = nullptr;
    ProgramOptions options = ProgramOptions::None;
    bool time = false;

    const wchar_t* data_folder = nullptr;
    const wchar_t* export_folder = nullptr;

    void parse() {
        ArgParser p;
        p.parse();

        for (const auto arg : p.arguments) {
            if (!source_file) {
                source_file = arg;
            } else if (!destination_file) {
                destination_file = arg;
            } else {
                wprintf(L"warning: unknown argument \"%s\"\n", arg);
            }
        }

        for (const auto flag : p.flags) {
            if (string_equals(flag, L"export-timestamp") || string_equals(flag, L"export-timestamps")) {
                options |= ProgramOptions::ExportTimestamp;
            } else if (string_equals(flag, L"time")) {
                time = true;
            } else if (string_equals(flag, L"preserve-record-order")) {
                options |= ProgramOptions::PreserveRecordOrder;
            } else if (string_equals(flag, L"preserve-junk")) {
                options |= ProgramOptions::PreserveJunk;
            } else if (string_equals(flag, L"debug-zlib")) {
                options |= ProgramOptions::DebugZLib;
            } else if (string_equals(flag, L"export-related-files")) {
                options |= ProgramOptions::ExportRelatedFiles;
            } else {
                wprintf(L"warning: unknown switch \"--%s\"\n", flag);
            }
        }

        for (const auto option : p.options) {
            if (string_equals(option.key, L"data-folder")) {
                data_folder = option.value;
            } else if (string_equals(option.key, L"export-folder")) {
                export_folder = option.value;
            } else {
                wprintf(L"warning: unknown option \"--%s=%s\"\n", option.key, option.value);
            }
        }
    }
};

static wchar_t* mwprintf(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    int count = _vsnwprintf(nullptr, 0, format, args);
    verify(count >= 0);
    va_end(args);

    auto buffer = new wchar_t[count + 1];

    va_start(args, format);
    int new_count = _vsnwprintf(buffer, count + 1, format, args);
    verify(new_count >= 0 && new_count <= count);
    va_end(args);

    buffer[new_count] = L'\0';
    return buffer;
}

static void push_if_not_duplicate(Array<const WString*>& paths, const WString* str) {
    if (str->count == 0) {
        return;
    }
    
    for (const auto path : paths) {
        if (path->count == str->count && 0 == _strnicmp(path->data, str->data, path->count)) {
            return;
        }
    }
    paths.push(str);
}

static void export_related_files(const Args& args, const wchar_t* esp_name, const Array<RecordBase*>& records) {
    auto data_path = args.data_folder && wcslen(args.data_folder) ? args.data_folder : path_append(get_skyrim_se_install_path(), L"Data");
    // @TODO: if export_folder == NULL, replace with GetCurrentDirectoryW()

    Array<FormID> facegens;
    Array<FormID> seq_formids;
    Array<const WString*> script_paths; // @TODO: Remove built-in scripts.

    foreach_record(records, [&facegens, esp_name, &seq_formids, &script_paths](Record* record) -> void {
        switch (record->type) {
            case RecordType::NPC_: {
                facegens.push(FormID{ record->id.value & 0x00ffffff });
            } break;

            case RecordType::QUST: {
                const auto dnam = record->find_field(RecordFieldType::DNAM);

                enum class Flags : uint16_t {
                    StartGameEnabled = 0x1,
                };

                if (dnam && dnam->data.count >= sizeof(Flags)) {
                    auto flags = *(Flags*)dnam->data.data;
                    if ((uint16_t)flags & (uint16_t)Flags::StartGameEnabled) {
                        seq_formids.push(record->id);
                    }
                }
            } break;
        }

        const auto vmad_field = record->find_field(RecordFieldType::VMAD);
        if (vmad_field) {
            VMAD_Field vmad;
            vmad.parse(vmad_field->data.data, vmad_field->data.count, record->type, true);

            for (const auto& script : vmad.scripts) {
                push_if_not_duplicate(script_paths, script.name);
            }

            if (vmad.contains_record_specific_info) {
                switch (record->type) {
                    case RecordType::INFO: {
                        if (is_bit_set(vmad.info.flags, PapyrusFragmentFlags::HasBeginScript)) {
                            push_if_not_duplicate(script_paths, vmad.info.start_fragment.script_name);
                        }
                        if (is_bit_set(vmad.info.flags, PapyrusFragmentFlags::HasEndScript)) {
                            push_if_not_duplicate(script_paths, vmad.info.end_fragment.script_name);
                        }
                    } break;

                    case RecordType::QUST: {
                        for (const auto& fragment : vmad.qust.fragments) {
                            push_if_not_duplicate(script_paths, fragment.script_name);
                        }
                        for (const auto& alias : vmad.qust.aliases) {
                            for (const auto& script : alias.scripts) {
                                push_if_not_duplicate(script_paths, script.name);
                            }
                        }
                    } break;
                }
            }
        }
    });

    Array<wchar_t*> paths;
    if (facegens.count > 0) {
        auto folder_path = path_append(args.export_folder, L"Textures\\Actors\\Character\\FaceGenData\\FaceTint\\");
        folder_path = path_append(folder_path, esp_name);
        create_folder(folder_path);

        for (const auto facegen : facegens) {
            auto path = path_append(L"Textures\\Actors\\Character\\FaceGenData\\FaceTint\\", esp_name);
            path = path_append(path, mwprintf(L"08X.dds", facegen.value));
            paths.push(path);
        }
    }

    // @TODO: Read paths from Creation Kit INI.
    if (script_paths.count > 0) {
        create_folder(path_append(args.export_folder, L"Scripts\\Source"));
        for (const auto script_path : script_paths) {
            paths.push(mwprintf(L"Scripts\\Source\\%.*S.psc", script_path->count, script_path->data));
            paths.push(mwprintf(L"Scripts\\%.*S.pex", script_path->count, script_path->data));
        }
    }

    for (const auto path : paths) {
        auto src_path = path_append(data_path, path);
        auto dst_path = path_append(args.export_folder, path);
        if (copy_file(src_path, dst_path)) {
            wprintf(L"> copied \"%s\" -> \"%s\"\n", src_path, dst_path);
        } else {
            wprintf(L"> error copying \"%s\" -> \"%s\": %s\n", src_path, dst_path, get_last_error());
        }
    }

    // @TODO: Write SEQ.
    // @TODO: Export DialogueViews.
    // @TODO: ESP to text: replace VMAD parsing to use "VMAD_Field".
}

int main() {
    Args args;
    args.parse();

    if (!args.source_file) {
        print_usage("<source file> argument is missing.\n");
        return 1;
    }

    const auto source_file = args.source_file;
    const auto source_file_extension = get_file_extension(source_file);
    const auto destination_file = args.destination_file ? args.destination_file : replace_destination_file_extension(source_file, source_file_extension);

    const auto start = args.time ? get_current_timestamp() : 0;

    if (string_equals(source_file_extension, L".txt")) {
        text_to_esp(source_file, destination_file);
    } else if (string_equals(source_file_extension, L".esp") || string_equals(source_file_extension, L".esm") || string_equals(source_file_extension, L".esl")) {
        EspParser parser;
        parser.init(args.options);
        defer(parser.dispose());
        
        const auto file = read_file(source_file);
        defer(delete[] file.data);

        parser.parse(file);

        if (is_bit_set(args.options, ProgramOptions::ExportRelatedFiles)) {
            // @TODO: remove hardcoded value
            export_related_files(args, L"FrostMawCave_miere.esp", parser.model.records);
        }

        esp_to_text(args.options, parser.model, destination_file);
    } else {
        exit_error(L"unrecognized source file extension \"%s\" (\"%s\")", source_file_extension, source_file);
    }

    if (args.time) {
        printf("Time elapsed: %f seconds\n", timestamp_to_seconds(start, get_current_timestamp()));
    }
    
    return 0;
}
