#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "esp_to_text.hpp"
#include "os.hpp"
#include "tes.hpp"
#include "common.hpp"
#include "typeinfo.hpp"
#include <stdio.h>
#include "base64.hpp"
#include <zlib.h>
#include "esp_parser.hpp"

struct TextRecordWriter {
    Slice scratch_buffer;

    HANDLE output_handle = 0;
    int indent = 0;
    bool localized_strings = false; // @TODO: load value from TES4 record

    const RecordBase* current_record = nullptr; // Sometimes ESP deserialization depends on record type.

    void open(const wchar_t* path) {
        verify(!output_handle);
        output_handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        verify(output_handle != INVALID_HANDLE_VALUE);

        scratch_buffer = allocate_virtual_memory(1024 * 1024 * 32);

        write_format("plugin2text version 1.00\n---\n");
    }

    void close() {
        verify(output_handle);
        CloseHandle(output_handle);
        output_handle = 0;
    }

    void write_format(_Printf_format_string_ const char* format, ...) {
        char buffer[4096];

        va_list args;
        va_start(args, format);
        int count = vsprintf_s(buffer, format, args);
        va_end(args);

        verify(count > 0);
        write_bytes(buffer, count);
    }

    void write_byte_array(const uint8_t* data, size_t size) {
        auto buffer = new uint8_t[size * 2];
        static const char alphabet[17] = "0123456789abcdef";

        for (size_t i = 0; i < size; ++i) {
            uint8_t c = data[i];
            buffer[(i * 2) + 0] = alphabet[c / 16];
            buffer[(i * 2) + 1] = alphabet[c % 16];
        }

        write_bytes(buffer, size * 2);
        delete[] buffer;
    }

    void write_indent() {
        for (int i = 0; i < indent; ++i) {
            write_bytes("  ", 2);
        }
    }

    void write_newline() {
        write_bytes("\n", 1);
    }

    void write_string(const char* str) {
        write_bytes(str, strlen(str));
    }

    template<size_t N>
    void write_literal(const char(&data)[N]) {
        write_bytes(data, N - 1);
    }

    void write_bytes(const void* data, size_t size) {
        // @TODO: add buffering
        DWORD written = 0;
        verify(WriteFile(output_handle, data, (uint32_t)size, &written, nullptr));
        verify(written == size);
    }

    void write_records(const Array<RecordBase*> records) {
        for (const auto record : records) {
            write_record(record);
        }
    }

    void write_grup_record(const GrupRecord* record) {
        if (record->group_type != RecordGroupType::Top) {
            write_format(" - %s", record_group_type_to_string(record->group_type));
            
            switch (record->group_type) {
                case RecordGroupType::ExteriorCellBlock:
                case RecordGroupType::ExteriorCellSubBlock: {
                    write_format(" (%d; %d)", record->grid_x, record->grid_y);
                } break;

                case RecordGroupType::InteriorCellBlock:
                case RecordGroupType::InteriorCellSubBlock: {
                    write_format(" %d", (int)record->label);
                } break;

                default: {
                    write_format(" [%08X]", record->label);
                } break;
            }
        }

        //constexpr bool ExportTimestamp = false;
        //if (ExportTimestamp) {
        //    write_record_timestamp(record->timestamp);
        //}
        //verify(!record->current_user_id);
        //verify(!record->last_user_id);

        write_record_unknown(record->unknown);

        write_newline();

        indent += 1;
        for (const auto record : record->records) {
            write_record(record);
        }
        indent -= 1;
    }

    RecordFlags write_flags(RecordFlags flags, const RecordDef* def) {
        for (size_t i = 0; i < def->flags.count; ++i) {
            const auto& flag = def->flags.data[i];
            if ((uint32_t)flags & flag.bit) {
                write_newline();
                write_indent();
                write_literal("+ ");
                write_string(flag.name);
                flags = clear_bit(flags, (RecordFlags)flag.bit);
            }
        }
        return flags;
    }

    void write_record_timestamp(uint16_t timestamp) {
        if (timestamp) {
            write_newline();
            indent += 1;
            write_indent();
            indent -= 1;
            int y = (timestamp & 0b1111111'0000'00000) >> 9;
            int m = (timestamp & 0b0000000'1111'00000) >> 5;
            int d = (timestamp & 0b0000000'0000'11111);
            verify(m >= 1 && m <= 12);
            write_format("%d %s 20%d", d, month_to_short_string(m), y);
        }
    }

    void write_record_unknown(uint32_t unknown) {
        if (unknown) {
            write_newline();
            indent += 1;
            write_indent();
            indent -= 1;
            write_format("Unknown = %X", unknown);
        }
    }

    void write_record(const RecordBase* record_base) {
        current_record = record_base;
        write_indent();
        write_bytes(&record_base->type, 4);

        if (record_base->type == RecordType::GRUP) {
            write_grup_record((const GrupRecord*)record_base);
            return;
        }

        auto record = (const Record*)record_base;
        write_format(" [%08X]", record->id.value);
        if (record->version != 44) {
            verify(false);
            //write_format(",v%d", record->version);
        }

        auto def = get_record_def(record->type);
        if (def && def->comment) {
            write_bytes(" - ", 3);
            write_bytes(def->comment, strlen(def->comment));
        }

        //write_record_timestamp(record->timestamp);

        //if (record->current_user_id) {
        //    verify(false);
        //}

        //if (record->last_user_id) {
        //    verify(false);
        //}

        write_record_unknown(record->unknown);

        if (record->flags != RecordFlags::None) {
            auto flags = record->flags;
            indent += 1;
            if (def) {
                flags = write_flags(flags, def);
            }
            flags = write_flags(flags, &Record_Common);
            if (flags != RecordFlags::None) {
                write_newline();
                write_indent();
                write_format("+ %X", flags);
            }
            indent -= 1;
        }

        if (!def) {
            def = &Record_Common;
        }

        write_newline();

        for (const auto field : record->fields) {
            write_field(def, field);
        }
    }

    static void validate_ascii(const char* now, size_t count) {
        const auto end = now + count;
        while (now < end) {
            char c = *now++;
            verify(c >= 32 && c < 127); // @TODO: Escape string
        }
    }

    static bool has_custom_indent_rules(TypeKind kind) {
        switch (kind) {
            case TypeKind::Struct:
            case TypeKind::Constant:
            case TypeKind::Filter:
            case TypeKind::Vector3:
            case TypeKind::VMAD:
                return true;
        }
        return false;
    }

    static size_t count_bytes(const uint8_t* start, size_t size, uint8_t byte) {
        for (size_t i = 0; i < size; ++i) {
            if (start[i] != byte) {
                return i;
            }
        }
        return size;
    }

    void write_papyrus_object(BinaryReader& r, const VMAD_Header* header) {
        verify(header->object_format == 2);
        auto value = r.read<VMAD_PropertyObjectV2>();
        write_custom_field("Form ID", value.form_id);
        write_custom_field("Alias", value.alias);
        verify(value.unused == 0);
    }

    void write_papyrus_scripts(BinaryReader& r, const VMAD_Header* header, uint16_t script_count) {
        for (int i = 0; i < script_count; ++i) {
            begin_custom_struct("Script");
            defer(end_custom_struct());

            write_custom_field("Name", r.advance_wstring());

            if (header->version >= 4) {
                auto status = r.read<uint8_t>();
                write_custom_field("Status", status);
            }

            auto property_count = r.read<uint16_t>();
            for (int prop_index = 0; prop_index < property_count; ++prop_index) {
                begin_custom_struct("Property");
                defer(end_custom_struct());

                write_custom_field("Name", r.advance_wstring());

                auto property_type = r.read<PapyrusPropertyType>();
                write_custom_field("Type", property_type);
                if (header->version >= 4) {
                    write_custom_field("Status", r.read<uint8_t>());
                }

                switch (property_type) {
                    case PapyrusPropertyType::Object: {
                        write_papyrus_object(r, header);
                    } break;

                    default: {
                        verify(false);
                    } break;
                }
            }
        }
    }

    void write_type(const Type* type, const void* value, size_t size) {
        indent += 1;

        if (!has_custom_indent_rules(type->kind)) {
            write_indent();
        }

        switch (type->kind) {
            case TypeKind::ZString: {
                if (size == 0) {
                    write_bytes("\"\"", 2);
                } else {
                    validate_ascii((char*)value, size - 1);
                    write_bytes("\"", 1);
                    write_bytes(value, size - 1);
                    write_bytes("\"", 1);
                }
            } break;

            case TypeKind::LString: {
                verify(!localized_strings);
                if (size == 0) {
                    write_bytes("\"\"", 2);
                } else {
                    write_bytes("\"", 1);
                    write_bytes(value, size - 1);
                    write_bytes("\"", 1);
                }
            } break;

            case TypeKind::WString: {
                auto wstr = (WString*)value;
                if (wstr->count == 0) {
                    write_literal("\"\"");
                } else {
                    validate_ascii((char*)wstr->data, wstr->count);
                    write_bytes("\"", 1);
                    write_bytes(wstr->data, wstr->count);
                    write_bytes("\"", 1);
                }
            } break;

            case TypeKind::ByteArray: {
                write_byte_array((uint8_t*)value, size);
            } break;

            case TypeKind::ByteArrayCompressed: {
                auto buffer = scratch_buffer.advance(size);

                auto compressed_size = static_cast<uLongf>(scratch_buffer.remaining_size());
                auto result = ::compress(buffer, &compressed_size, (const uint8_t*)value, static_cast<uLong>(size));
                verify(result == Z_OK);

                scratch_buffer.now += compressed_size;

                write_format("%X ", (uint32_t)size);
                auto output_size = base64_encode(buffer, compressed_size, (char*)scratch_buffer.now, scratch_buffer.end - scratch_buffer.now);
                write_bytes(scratch_buffer.now, output_size);
                
                write_byte_array(buffer, compressed_size);

                scratch_buffer.now = buffer;
            } break;

            case TypeKind::ByteArrayFixed: {
                verify(type->size == size);
                write_byte_array((uint8_t*)value, size);
            } break;

            case TypeKind::ByteArrayRLE: {
                auto buffer = new uint8_t[size * 2];
                auto data = (uint8_t*)value;
                static const char alphabet[17] = "0123456789abcdef";
                size_t bytes_written = 0;

                for (size_t i = 0; i < size; ++i) {
                    uint8_t c = data[i];
                    if ((c == 0x00 || c == 0xFF) && i + 1 < size) {
                        size_t repeats = 1 + count_bytes(&data[i + 1], size - 1, c);
                        if (repeats > 1) {
                            if (repeats > ByteArrayRLE_MaxStreamValue) {
                                repeats = ByteArrayRLE_MaxStreamValue;
                            }
                         
                            buffer[bytes_written + 0] = c == 0x00 ? ByteArrayRLE_SequenceMarker_00 : ByteArrayRLE_SequenceMarker_FF;
                            buffer[bytes_written + 1] = ByteArrayRLE_StreamStart + (repeats - 1);
                            bytes_written += 2;
                            i += repeats - 1;
                            continue;
                        }
                    }

                    buffer[bytes_written + 0] = alphabet[c / 16];
                    buffer[bytes_written + 1] = alphabet[c % 16];
                    bytes_written += 2;
                }

                write_bytes(buffer, bytes_written);
                delete[] buffer;
            } break;

            case TypeKind::Integer: {
                verify(type->size == size);
                auto integer_type = (const TypeInteger*)type;
                if (integer_type->is_unsigned) {
                    switch (size) {
                        case 1: write_format("%u", *(uint8_t*)value); break;
                        case 2: write_format("%u", *(uint16_t*)value); break;
                        case 4: write_format("%u", *(uint32_t*)value); break;
                        case 8: write_format("%llu", *(uint64_t*)value); break;
                    }
                } else {
                    switch (size) {
                        case 1: write_format("%d", *(int8_t*)value); break;
                        case 2: write_format("%d", *(int16_t*)value); break;
                        case 4: write_format("%d", *(int32_t*)value); break;
                        case 8: write_format("%lld", *(int64_t*)value); break;
                    }
                }
            } break;

            case TypeKind::Float: {
                verify(type->size == size);
                switch (size) {
                    case sizeof(float) : {
                        write_format("%f", *(float*)value);
                    } break;

                    case sizeof(double) : {
                        write_format("%f", *(double*)value);
                    } break;
                }
            } break;

            case TypeKind::Struct: {
                verify(type->size == size);
                auto struct_type = (const TypeStruct*)type;
                size_t offset = 0;
                for (int i = 0; i < struct_type->field_count; ++i) {
                    const auto& field = struct_type->fields[i];
                    if (field.type->kind == TypeKind::Constant) {
                        write_type(field.type, (uint8_t*)value + offset, field.type->size);
                        offset += field.type->size;
                        continue;
                    }

                    write_indent();
                    write_bytes(field.name, strlen(field.name));
                    write_newline();

                    write_type(field.type, (uint8_t*)value + offset, field.type->size);
                    offset += field.type->size;
                }
                verify(offset == size);
            } break;

            case TypeKind::FormID: {
                verify(type->size == size);
                verify(size == sizeof(int));
                write_format("[%08X]", *(int*)value);
            } break;

            case TypeKind::FormIDArray: {
                verify((size % sizeof(int)) == 0);
                const size_t keyword_count = size / sizeof(int);
                for (size_t i = 0; i < keyword_count; ++i) {
                    write_format("[%08X]", ((int*)value)[i]);
                    if (i != keyword_count - 1) {
                        write_newline();
                        write_indent();
                    }
                }
            } break;

            case TypeKind::Enum: {
                verify(type->size == size);
                const auto enum_type = (const TypeEnum*)type;
                
                uint32_t enum_value = 0;
                switch (size) {
                    case 1: enum_value = *(uint8_t*)value; break;
                    case 2: enum_value = *(uint16_t*)value; break;
                    case 4: enum_value = *(uint32_t*)value; break;
                    case 8: verify(false); break; //enum_value = *(uint64_t*)value; break;
                    default: verify(false); break;
                }

                if (enum_type->flags) {
                    verify(type->size <= sizeof(uint32_t));

                    for (size_t i = 0; i < enum_type->field_count; ++i) {
                        const auto& field = enum_type->fields[i];
                        if (enum_value & field.value) {
                            write_literal("+ ");
                            write_string(field.name);

                            enum_value = clear_bit(enum_value, field.value);
                            if (enum_value == 0) {
                                break; // fast exit if wrote all bits.
                            } else {
                                write_newline();
                                write_indent();
                            }
                        }
                    }

                    if (enum_value) {
                        write_format("+ %X", enum_value);
                    }
                } else {
                    for (size_t i = 0; i < enum_type->field_count; ++i) {
                        const auto& field = enum_type->fields[i];
                        if (field.value == enum_value) {
                            write_bytes(field.name, strlen(field.name));
                            goto ok;
                        }
                    }

                    write_format("%u", enum_value);
                    ok: break;
                }
            } break;

            case TypeKind::Boolean: {
                verify(type->size == sizeof(bool));
                if (*(bool*)value) {
                    write_literal("True");
                } else {
                    write_literal("False");
                }
            } break;

            case TypeKind::VMAD: {
                BinaryReader r;
                r.start = (uint8_t*)value;
                r.now = r.start;
                r.end = r.start + size;

                auto now = (uint8_t*)value;

                auto header = r.advance<VMAD_Header>();
                verify(header->version >= 2 && header->version <= 5);
                verify(header->object_format >= 1 && header->object_format <= 2);

                write_custom_field("Version", header->version);
                write_custom_field("Object Format", header->object_format);

                write_papyrus_scripts(r, header, header->script_count);

                if (r.now != r.end) {
                    // @NOTE: instead of using "current_record" we can make VMAD_INFO, VMAD_QUST, etc...
                    switch (current_record->type) {
                        case RecordType::INFO: {
                            verify(r.read<uint8_t>() == 2); // version?

                            auto flags = r.read<PapyrusFragmentFlags>();
                            write_custom_field("Fragment Script File Name", r.advance_wstring());

                            // @TODO: copypaste
                            if ((uint8_t)flags & (uint8_t)PapyrusFragmentFlags::HasBeginScript) {
                                begin_custom_struct("Start Fragment");
                                defer(end_custom_struct());

                                verify(1 == r.read<uint8_t>());
                                write_custom_field("Script Name", r.advance_wstring());
                                write_custom_field("Fragment Name", r.advance_wstring());
                            }

                            if ((uint8_t)flags & (uint8_t)PapyrusFragmentFlags::HasEndScript) {
                                begin_custom_struct("End Fragment");
                                defer(end_custom_struct());

                                verify(1 == r.read<uint8_t>());
                                write_custom_field("Script Name", r.advance_wstring());
                                write_custom_field("Fragment Name", r.advance_wstring());
                            }
                        } break;

                        case RecordType::QUST: {
                            verify(r.read<uint8_t>() == 2); // version?

                            auto fragment_count = (int)r.read<uint16_t>();
                            write_custom_field("File Name", r.advance_wstring());

                            for (int frag_index = 0; frag_index < fragment_count; ++frag_index) {
                                begin_custom_struct("Fragment");
                                defer(end_custom_struct());

                                write_custom_field("Index", r.read<uint16_t>());
                                verify(0 == r.read<uint16_t>());
                                write_custom_field("Log Entry", r.read<uint32_t>());
                                verify(1 == r.read<uint8_t>());
                                write_custom_field("Script Name", r.advance_wstring());
                                write_custom_field("Function Name", r.advance_wstring());
                            };

                            auto alias_count = (int)r.read<uint16_t>();
                            for (int alias_index = 0; alias_index < alias_count; ++alias_index) {
                                begin_custom_struct("Alias");
                                defer(end_custom_struct());

                                write_papyrus_object(r, header);
                                verify(r.read<uint16_t>() == header->version);
                                verify(r.read<uint16_t>() == header->object_format);

                                const auto script_count = r.read<uint16_t>();
                                write_papyrus_scripts(r, header, script_count);
                            }
                        } break;

                        default: {
                            verify(false);
                        } break;
                    }
                }

                verify(r.now == r.end);
            } break;

            case TypeKind::Constant: {
                verify(type->size == size);
                const auto constant_type = (const TypeConstant*)type;
                verify(memory_equals(value, constant_type->bytes, size));
            } break;

            case TypeKind::Filter: {
                const auto filter_type = (const TypeFilter*)type;
                auto highwater = scratch_buffer.now;
                auto preprocessed_value = scratch_buffer.advance(size);
                memcpy(preprocessed_value, value, size);

                filter_type->preprocess(preprocessed_value, size);
                indent -= 1;
                write_type(filter_type->inner_type, preprocessed_value, size);
                indent += 1;

                scratch_buffer.now = highwater;
            } break;

            case TypeKind::Vector3: {
                // @TODO: maybe can replace with fixed array type.
                verify(size == sizeof(Vector3));
                const auto vector = (const Vector3*)value;
                indent -= 1;
                write_type(&Type_float, &vector->x, sizeof(vector->x));
                write_type(&Type_float, &vector->y, sizeof(vector->y));
                write_type(&Type_float, &vector->z, sizeof(vector->z));
                indent += 1;
            } break;

            default: {
                verify(false);
            } break;
        }
   
        if (!has_custom_indent_rules(type->kind)) {
            write_newline();
        }

        indent -= 1;
    }

    void write_field(const RecordDef* def, const RecordField* field) {
        indent += 1;
        write_indent();

        write_bytes(&field->type, 4);
        auto field_def = def->get_field_def(field->type);
        if (!field_def) {
            field_def = Record_Common.get_field_def(field->type);
        }

        if (field_def && field_def->comment) {
            write_bytes(" - ", 3);
            write_bytes(field_def->comment, strlen(field_def->comment));
        }

        write_newline();

        const auto data_type = field_def ? field_def->data_type : &Type_ByteArray;
        write_type(data_type, field->data.data, field->data.count);
        
        indent -= 1;
    }

    void begin_custom_struct(const char* header_name) {
        write_indent();
        write_string(header_name);
        write_newline();
        ++indent;
    }

    void end_custom_struct() {
        --indent;
    }

    void write_custom_field(const char* field_name, const Type* type, const void* value, size_t size) {
        write_indent();
        write_string(field_name);
        write_newline();
        write_type(type, value, size);
    }

    template<typename T>
    void write_custom_field(const char* field_name, const T* value) {
        write_custom_field(field_name, resolve_type<T>(), value, sizeof(T));
    }

    template<typename T>
    void write_custom_field(const char* field_name, const T& value) {
        write_custom_field(field_name, resolve_type<T>(), &value, sizeof(T));
    }
};

void esp_to_text(const EspObjectModel& model, const wchar_t* text_path) {
    TextRecordWriter writer;
    writer.open(text_path);

    uint32_t size = 0;
    writer.write_records(model.records);

    writer.close();
}
