#include "dart_bindings.h"

#include <functional>
#include <iostream>
#include <string.h>
#include <thread>

#include <dart_api.h>
#include <dart_dll.h>
#include <godot/gdextension_interface.h>

#include "dart_vtable_wrapper.h"
#include "gde_wrapper.h"

#define GDE GDEWrapper::instance()->gde()

void GodotDartBindings::thread_callback(GodotDartBindings *bindings) {
  bindings->thread_main();
}

/* Binding callbacks (not sure what these are for?) */

static void *__binding_create_callback(void *p_token, void *p_instance) {
  return nullptr;
}

static void __binding_free_callback(void *p_token, void *p_instance, void *p_binding) {
}

static GDExtensionBool __binding_reference_callback(void *p_token, void *p_instance, GDExtensionBool p_reference) {
  return true;
}

static constexpr GDExtensionInstanceBindingCallbacks __binding_callbacks = {
    __binding_create_callback,
    __binding_free_callback,
    __binding_reference_callback,
};

struct MethodInfo {
  std::string method_name;
  TypeInfo return_type;
  std::vector<TypeInfo> arguments;
};

GodotDartBindings *GodotDartBindings::_instance = nullptr;
Dart_NativeFunction native_resolver(Dart_Handle name, int num_of_arguments, bool *auto_setup_scope);

bool GodotDartBindings::initialize(const char *script_path, const char *package_config) {
  dart_vtable_wrapper::init_virtual_thunks();

  DartDll_Initialize();

  _isolate = DartDll_LoadScript(script_path, package_config);
  if (_isolate == nullptr) {
    GD_PRINT_ERROR("GodotDart: Initialization Error (Failed to load script)");
    return false;
  }

  Dart_EnterIsolate(_isolate);
  Dart_EnterScope();

  Dart_Handle godot_dart_package_name = Dart_NewStringFromCString("package:godot_dart/godot_dart.dart");
  Dart_Handle godot_dart_library = Dart_LookupLibrary(godot_dart_package_name);
  if (Dart_IsError(godot_dart_library)) {
    GD_PRINT_ERROR("GodotDart: Initialization Error (Could not find the `godot_dart` "
                   "package)");
    return false;
  } else {
    _godot_dart_library = Dart_NewPersistentHandle(godot_dart_library);
  }

  // Find the DartBindings "library" (just the file) and set us as the native callback handler
  {
    Dart_Handle native_bindings_library_name =
        Dart_NewStringFromCString("package:godot_dart/src/core/godot_dart_native_bindings.dart");
    Dart_Handle library = Dart_LookupLibrary(native_bindings_library_name);
    if (!Dart_IsError(library)) {
      // Retrain for future calls to convert variants
      _native_library = Dart_NewPersistentHandle(library);
      Dart_SetNativeResolver(library, native_resolver, nullptr);
    }
  }

  // Find the DartBindings "library" (just the file) and set us as the native callback handler
  {
    Dart_Handle native_bindings_library_name = Dart_NewStringFromCString("package:godot_dart/src/core/core_types.dart");
    Dart_Handle library = Dart_LookupLibrary(native_bindings_library_name);
    if (!Dart_IsError(library)) {
      // Retrain for future calls to convert variants
      _core_types_library = Dart_NewPersistentHandle(library);
      Dart_SetNativeResolver(library, native_resolver, nullptr);
    }
  }

  // Setup some types we need frequently
  {
    Dart_Handle library = Dart_LookupLibrary(Dart_NewStringFromCString("dart:ffi"));
    if (Dart_IsError(library)) {
      GD_PRINT_ERROR("GodotDart: Error getting ffi library: ");
      GD_PRINT_ERROR(Dart_GetError(library));

      return false;
    }
    Dart_Handle dart_void = Dart_GetNonNullableType(library, Dart_NewStringFromCString("Void"), 0, nullptr);
    if (Dart_IsError(dart_void)) {
      GD_PRINT_ERROR("GodotDart: Error getting Void type: ");
      GD_PRINT_ERROR(Dart_GetError(dart_void));

      return false;
    }
    Dart_Handle type_args = Dart_NewList(1);
    if (Dart_IsError(type_args)) {
      GD_PRINT_ERROR("GodotDart: Could not create arg list ");
      GD_PRINT_ERROR(Dart_GetError(type_args));

      return false;
    }
    Dart_ListSetAt(type_args, 0, dart_void);
    Dart_Handle void_pointer = Dart_GetNonNullableType(library, Dart_NewStringFromCString("Pointer"), 1, &type_args);
    if (Dart_IsError(void_pointer)) {
      GD_PRINT_ERROR("GodotDart: Error getting Pointer<Void> type: ");
      GD_PRINT_ERROR(Dart_GetError(void_pointer));

      return false;
    }
    _void_pointer_type = Dart_NewPersistentHandle(void_pointer);

    Dart_Handle optional_void_pointer =
        Dart_GetNullableType(library, Dart_NewStringFromCString("Pointer"), 1, &type_args);
    if (Dart_IsError(void_pointer)) {
      GD_PRINT_ERROR("GodotDart: Error getting Pointer<Void>? type: ");
      GD_PRINT_ERROR(Dart_GetError(optional_void_pointer));

      return false;
    }
    _void_pointer_optional_type = Dart_NewPersistentHandle(optional_void_pointer);

    Dart_Handle type_args_2 = Dart_NewList(1);
    Dart_ListSetAt(type_args_2, 0, void_pointer);
    Dart_Handle pointer_to_pointer =
        Dart_GetNonNullableType(library, Dart_NewStringFromCString("Pointer"), 1, &type_args_2);
    if (Dart_IsError(pointer_to_pointer)) {
      GD_PRINT_ERROR("GodotDart: Error getting Pointer<Pointer<Void>> type: ");
      GD_PRINT_ERROR(Dart_GetError(pointer_to_pointer));

      return false;
    }
    _void_pointer_pointer_type = Dart_NewPersistentHandle(pointer_to_pointer);
  }

  // All set up, setup the instance
  _instance = this;

  // Everything should be prepared, register Dart with Godot
  {
    GDEWrapper *wrapper = GDEWrapper::instance();
    Dart_Handle args[] = {Dart_NewInteger((int64_t)wrapper->gde()), Dart_NewInteger((int64_t)wrapper->lib())};
    Dart_Handle result = Dart_Invoke(godot_dart_library, Dart_NewStringFromCString("_registerGodot"), 2, args);
    if (Dart_IsError(result)) {
      GD_PRINT_ERROR("GodotDart: Error calling `_registerGodot`");
      GD_PRINT_ERROR(Dart_GetError(result));
      return false;
    }
  }

  // And call the main function from the user supplied library
  {
    Dart_Handle library = Dart_RootLibrary();
    Dart_Handle mainFunctionName = Dart_NewStringFromCString("main");
    Dart_Handle result = Dart_Invoke(library, mainFunctionName, 0, nullptr);
    if (Dart_IsError(result)) {
      GD_PRINT_ERROR("GodotDart: Error calling `main`");
      GD_PRINT_ERROR(Dart_GetError(result));
      return false;
    }
  }

  Dart_ExitScope();

  // Create a thread for doing Dart work
  Dart_ExitIsolate();
  _dart_thread = new std::thread(GodotDartBindings::thread_callback, this);

  return true;
}

void GodotDartBindings::shutdown() {
  _stopRequested = true;
  execute_on_dart_thread([]() {});

  Dart_EnterIsolate(_isolate);
  Dart_EnterScope();

  Dart_Handle godot_dart_library = Dart_HandleFromPersistent(_godot_dart_library);

  GDEWrapper *wrapper = GDEWrapper::instance();
  Dart_Handle result = Dart_Invoke(godot_dart_library, Dart_NewStringFromCString("_unregisterGodot"), 0, nullptr);
  if (Dart_IsError(result)) {
    GD_PRINT_ERROR("GodotDart: Error calling `_unregisterGodot`");
    GD_PRINT_ERROR(Dart_GetError(result));
  }

  Dart_DeletePersistentHandle(_godot_dart_library);
  Dart_DeletePersistentHandle(_core_types_library);
  Dart_DeletePersistentHandle(_native_library);

  DartDll_DrainMicrotaskQueue();
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  DartDll_Shutdown();

  _instance = nullptr;
}

void GodotDartBindings::thread_main() {

  Dart_EnterIsolate(_isolate);

  while (!_stopRequested) {
    _work_semaphore.acquire();

    _pendingWork();
    _pendingWork = []() {};

    // Do work
    _done_semaphore.release();
  }

  Dart_ExitIsolate();
}

void GodotDartBindings::execute_on_dart_thread(std::function<void()> work) {
  if (_dart_thread == nullptr || std::this_thread::get_id() == _dart_thread->get_id()) {
    work();
    return;
  }

  _work_lock.lock();

  _pendingWork = work;
  _work_semaphore.release();
  _done_semaphore.acquire();

  _work_lock.unlock();
}

void GodotDartBindings::bind_method(const TypeInfo &bind_type, const char *method_name, const TypeInfo &ret_type_info,
                                    const std::vector<TypeInfo> &arg_list) {
  MethodInfo *info = new MethodInfo();
  info->method_name = method_name;
  info->return_type = ret_type_info;
  info->arguments = arg_list;

  GDEWrapper *gde = GDEWrapper::instance();

  uint8_t gd_empty_string[GD_STRING_NAME_MAX_SIZE];
  GDE->string_new_with_utf8_chars(&gd_empty_string, "");

  GDExtensionPropertyInfo ret_info = {
      ret_type_info.variant_type,
      ret_type_info.type_name,
      gd_empty_string,
      0, // Hint - String
      gd_empty_string,
      6, // Usage - PROPERTY_USAGE_DEFAULT,
  };

  // Parameters / Metadata
  GDExtensionPropertyInfo *arg_info = new GDExtensionPropertyInfo[arg_list.size()];
  GDExtensionClassMethodArgumentMetadata *arg_meta_info = new GDExtensionClassMethodArgumentMetadata[arg_list.size()];
  for (size_t i = 0; i < arg_list.size(); ++i) {
    arg_info[i].class_name = arg_list[i].type_name;
    arg_info[i].hint = 0;
    arg_info[i].hint_string = gd_empty_string;
    arg_info[i].name = gd_empty_string, arg_info[i].usage = 6,

    // TODO - actually need this to specify int / double size
        arg_meta_info[i] = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
  }

  int flags = GDEXTENSION_METHOD_FLAG_NORMAL;
  // TODO: Pass in virtual flag
  if (method_name[0] == '_') {
    flags |= GDEXTENSION_METHOD_FLAG_VIRTUAL;
  }

  uint8_t gd_method_name[GD_STRING_NAME_MAX_SIZE];
  gde->gd_string_name_new(&gd_method_name, method_name);
  GDExtensionClassMethodInfo method_info = {
      gd_method_name,
      info,
      GodotDartBindings::bind_call,
      GodotDartBindings::ptr_call,
      flags,
      ret_type_info.variant_type != GDEXTENSION_VARIANT_TYPE_NIL,
      &ret_info,
      GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE,
      arg_list.size(),
      arg_info,
      arg_meta_info,
      0,
      nullptr,
  };

  GDE->classdb_register_extension_class_method(gde->lib(), bind_type.type_name, &method_info);

  delete[] arg_info;
  delete[] arg_meta_info;
}

void *get_opaque_address(Dart_Handle variant_handle) {
  // TODO: Look for a better way convert the variant.
  Dart_Handle opaque = Dart_GetField(variant_handle, Dart_NewStringFromCString("_opaque"));
  if (Dart_IsError(opaque)) {
    GD_PRINT_ERROR(Dart_GetError(opaque));
    return nullptr;
  }
  Dart_Handle address = Dart_GetField(opaque, Dart_NewStringFromCString("address"));
  if (Dart_IsError(address)) {
    GD_PRINT_ERROR(Dart_GetError(address));
    return nullptr;
  }
  uint64_t variantDataPtr = 0;
  Dart_IntegerToUint64(address, &variantDataPtr);

  return reinterpret_cast<void *>(variantDataPtr);
}

void type_info_from_dart(TypeInfo *type_info, Dart_Handle dart_type_info) {
  Dart_EnterScope();

  Dart_Handle class_name = Dart_GetField(dart_type_info, Dart_NewStringFromCString("className"));
  Dart_Handle parent_class = Dart_GetField(dart_type_info, Dart_NewStringFromCString("parentClass"));
  Dart_Handle variant_type = Dart_GetField(dart_type_info, Dart_NewStringFromCString("variantType"));
  Dart_Handle bindings_ptr = Dart_GetField(dart_type_info, Dart_NewStringFromCString("bindingCallbacks"));

  type_info->type_name = get_opaque_address(class_name);
  if (Dart_IsNull(parent_class)) {
    type_info->parent_name = nullptr;
  } else {
    type_info->parent_name = get_opaque_address(parent_class);
  }
  int64_t temp;
  Dart_IntegerToInt64(variant_type, &temp);
  type_info->variant_type = static_cast<GDExtensionVariantType>(temp);
  if (Dart_IsNull(bindings_ptr)) {
    type_info->binding_callbacks = nullptr;
  } else {
    Dart_Handle bindings_address = Dart_GetField(bindings_ptr, Dart_NewStringFromCString("address"));
    if (Dart_IsError(bindings_address)) {
      GD_PRINT_ERROR(Dart_GetError(bindings_address));
      type_info->binding_callbacks = nullptr;
    } else {
      uint64_t bindings_c_ptr = 0;
      Dart_IntegerToUint64(bindings_address, &bindings_c_ptr);
      type_info->binding_callbacks = reinterpret_cast<GDExtensionInstanceBindingCallbacks *>(bindings_c_ptr);
    }
  }

  Dart_ExitScope();
}

/* Static Callbacks from Godot */

void GodotDartBindings::bind_call(void *method_userdata, GDExtensionClassInstancePtr instance,
                                  const GDExtensionConstVariantPtr *args, GDExtensionInt argument_count,
                                  GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    // oooff
    return;
  }

  bindings->execute_on_dart_thread([&]() {
    Dart_EnterScope();

    Dart_PersistentHandle persist_handle = reinterpret_cast<Dart_PersistentHandle>(instance);
    Dart_Handle dart_instance = Dart_HandleFromPersistent(persist_handle);

    MethodInfo *method_info = reinterpret_cast<MethodInfo *>(method_userdata);
    Dart_Handle dart_method_name = Dart_NewStringFromCString(method_info->method_name.c_str());

    Dart_Handle *dart_args = nullptr;
    if (method_info->arguments.size() > 0) {
      // First convert to Dart values
      // Get all the bindings callbacks for the requested parameters
      Dart_Handle dart_bindings_list =
          Dart_NewListOfTypeFilled(bindings->_void_pointer_optional_type, Dart_Null(), method_info->arguments.size());
      for (size_t i = 0; i < method_info->arguments.size(); ++i) {
        const TypeInfo &arg_info = method_info->arguments[i];
        if (arg_info.binding_callbacks != nullptr) {
          Dart_Handle callbacks_address = Dart_NewInteger(reinterpret_cast<intptr_t>(args));
          Dart_Handle callbacks_ptr = Dart_New(Dart_HandleFromPersistent(bindings->_void_pointer_pointer_type),
                                               Dart_NewStringFromCString("fromAddress"), 1, &callbacks_address);
          Dart_ListSetAt(dart_bindings_list, i, callbacks_ptr);
        }
      }

      Dart_Handle args_address = Dart_NewInteger(reinterpret_cast<intptr_t>(args));
      Dart_Handle convert_args[3]{
          Dart_New(Dart_HandleFromPersistent(bindings->_void_pointer_pointer_type),
                   Dart_NewStringFromCString("fromAddress"), 1, &args_address),
          Dart_NewInteger(method_info->arguments.size()),
          dart_bindings_list,
      };
      if (Dart_IsError(convert_args[0])) {
        GD_PRINT_ERROR("GodotDart: Error creating parameters: ");
        GD_PRINT_ERROR(Dart_GetError(convert_args[0]));

        Dart_ExitScope();
        return;
      }
      Dart_Handle dart_arg_list =
          Dart_Invoke(bindings->_native_library, Dart_NewStringFromCString("_variantsToDart"), 3, convert_args);
      if (Dart_IsError(dart_arg_list)) {
        GD_PRINT_ERROR("GodotDart: Error converting parameters from Variants: ");
        GD_PRINT_ERROR(Dart_GetError(dart_arg_list));

        Dart_ExitScope();
        return;
      }

      dart_args = new Dart_Handle[method_info->arguments.size()];
      for (size_t i = 0; i < method_info->arguments.size(); ++i) {
        dart_args[i] = Dart_ListGetAt(dart_arg_list, i);
      }
    }

    Dart_Handle result = Dart_Invoke(dart_instance, dart_method_name, method_info->arguments.size(), dart_args);
    if (Dart_IsError(result)) {
      GD_PRINT_ERROR("GodotDart: Error calling function: ");
      GD_PRINT_ERROR(Dart_GetError(result));
    } else {
      // Call back into Dart to convert to Variant. This may get moved back into C at some point but
      // the logic and type checking is easier in Dart.
      Dart_Handle native_library = Dart_HandleFromPersistent(bindings->_native_library);
      Dart_Handle args[] = {result};
      Dart_Handle variant_result = Dart_Invoke(native_library, Dart_NewStringFromCString("_convertToVariant"), 1, args);
      if (Dart_IsError(variant_result)) {
        GD_PRINT_ERROR("GodotDart: Error converting return to variant: ");
        GD_PRINT_ERROR(Dart_GetError(result));
      } else {
        void *variantDataPtr = get_opaque_address(variant_result);
        if (variantDataPtr) {
          GDE->variant_new_copy(r_return, reinterpret_cast<GDExtensionConstVariantPtr>(variantDataPtr));
        }
      }
    }

    if (dart_args != nullptr) {
      delete[] dart_args;
    }

    Dart_ExitScope();
  });
}

void GodotDartBindings::ptr_call(void *method_userdata, GDExtensionClassInstancePtr instance,
                                 const GDExtensionConstVariantPtr *args, GDExtensionVariantPtr r_return) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    // oooff
    return;
  }

  bindings->execute_on_dart_thread([&]() {
    Dart_EnterScope();

    // Not implemented yet (haven't come across an instance of it yet?)

    Dart_ExitScope();
  });
}

GDExtensionClassCallVirtual GodotDartBindings::get_virtual_func(void *p_userdata,
                                                                GDExtensionConstStringNamePtr p_name) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    // oooff
    return nullptr;
  }

  GDExtensionClassCallVirtual func = nullptr;
  bindings->execute_on_dart_thread([&]() {
    Dart_EnterScope();

    Dart_Handle type = Dart_HandleFromPersistent(reinterpret_cast<Dart_PersistentHandle>(p_userdata));

    Dart_Handle vtable = Dart_GetField(type, Dart_NewStringFromCString("vTable"));
    if (Dart_IsError(vtable)) {
      GD_PRINT_ERROR("GodotDart: Error finding typeInfo on object: ");
      GD_PRINT_ERROR(Dart_GetError(vtable));
      Dart_ExitScope();
      return;
    }

    if (Dart_IsNull(vtable)) {
      Dart_ExitScope();
      return;
    }

    // TODO: Maybe we can use StringNames directly instead of converting to Dart strings?
    GDEWrapper *gde = GDEWrapper::instance();
    uint8_t gd_string[GD_STRING_MAX_SIZE] = {0};
    gde->gd_string_from_string_name(p_name, gd_string);

    char16_t length = GDE->string_to_utf16_chars(gd_string, nullptr, 0);
    char16_t *temp = (char16_t *)_alloca(sizeof(char16_t) * (length + 1));
    GDE->string_to_utf16_chars(gd_string, temp, length);
    temp[length] = 0;

    gde->gd_string_destructor(gd_string);

    Dart_Handle dart_string = Dart_NewStringFromUTF16((uint16_t *)temp, length);
    if (Dart_IsError(dart_string)) {
      GD_PRINT_ERROR("GodotDart: Error conveting StringName to Dart String: ");
      GD_PRINT_ERROR(Dart_GetError(dart_string));
      Dart_ExitScope();
      return;
    }

    Dart_Handle vtable_item = Dart_MapGetAt(vtable, dart_string);
    if (Dart_IsError(vtable_item)) {
      GD_PRINT_ERROR("GodotDart: Error looking for vtable item: ");
      GD_PRINT_ERROR(Dart_GetError(vtable_item));
      Dart_ExitScope();
      return;
    }

    if (Dart_IsNull(vtable_item)) {
      Dart_ExitScope();
      return;
    }

    Dart_Handle dart_address = Dart_GetField(vtable_item, Dart_NewStringFromCString("address"));

    uint64_t address = 0;
    Dart_IntegerToUint64(dart_address, &address);

    func = dart_vtable_wrapper::get_wrapped_virtual(reinterpret_cast<GDExtensionClassCallVirtual>(address));

    Dart_ExitScope();
  });

  return func;
}

GDExtensionObjectPtr GodotDartBindings::class_create_instance(void *p_userdata) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    // oooff
    return nullptr;
  }

  uint64_t real_address = 0;
  bindings->execute_on_dart_thread([&]() {
    Dart_EnterScope();

    Dart_Handle type = Dart_HandleFromPersistent(reinterpret_cast<Dart_PersistentHandle>(p_userdata));

    Dart_Handle d_class_type_info = Dart_GetField(type, Dart_NewStringFromCString("typeInfo"));
    if (Dart_IsError(d_class_type_info)) {
      GD_PRINT_ERROR("GodotDart: Error finding typeInfo on object: ");
      GD_PRINT_ERROR(Dart_GetError(d_class_type_info));
      Dart_ExitScope();
      return;
    }
    TypeInfo class_type_info;
    type_info_from_dart(&class_type_info, d_class_type_info);

    Dart_Handle new_object = Dart_New(type, Dart_Null(), 0, nullptr);
    if (Dart_IsError(new_object)) {
      GD_PRINT_ERROR("GodotDart: Error creating object: ");
      GD_PRINT_ERROR(Dart_GetError(new_object));
      Dart_ExitScope();
      return;
    }

    Dart_Handle owner = Dart_GetField(new_object, Dart_NewStringFromCString("nativePtr"));
    if (Dart_IsError(owner)) {
      GD_PRINT_ERROR("GodotDart: Error finding owner member for object: ");
      GD_PRINT_ERROR(Dart_GetError(owner));
      Dart_ExitScope();
      return;
    }

    Dart_Handle owner_address = Dart_GetField(owner, Dart_NewStringFromCString("address"));
    if (Dart_IsError(owner_address)) {
      GD_PRINT_ERROR("GodotDart: Error getting address for object: ");
      GD_PRINT_ERROR(Dart_GetError(owner_address));
      Dart_ExitScope();
      return;
    }

    Dart_IntegerToUint64(owner_address, &real_address);

    Dart_ExitScope();
  });

  return reinterpret_cast<GDExtensionObjectPtr>(real_address);
}

void GodotDartBindings::class_free_instance(void *p_userdata, GDExtensionClassInstancePtr p_instance) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    // oooff
    return;
  }

  bindings->execute_on_dart_thread(
      [&]() { Dart_DeletePersistentHandle(reinterpret_cast<Dart_PersistentHandle>(p_instance)); });
}

/* Static Functions From Dart */

void bind_class(Dart_NativeArguments args) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    Dart_ThrowException(Dart_NewStringFromCString("GodotDart has been shutdown!"));
    return;
  }

  Dart_Handle type_arg = Dart_GetNativeArgument(args, 1);
  Dart_Handle type_info = Dart_GetNativeArgument(args, 2);

  // Name and Parent are StringNames and we can get their opaque addresses
  Dart_Handle name = Dart_GetField(type_info, Dart_NewStringFromCString("className"));
  Dart_Handle parent = Dart_GetField(type_info, Dart_NewStringFromCString("parentClass"));
  if (Dart_IsNull(parent)) {
    Dart_ThrowException(Dart_NewStringFromCString("Passed null reference for parent in bindClass."));
    return;
  }

  void *sn_name = get_opaque_address(name);
  if (sn_name == nullptr) {
    return;
  }
  void *sn_parent = get_opaque_address(parent);
  if (sn_parent == nullptr) {
    return;
  }

  GDExtensionClassCreationInfo info = {0};
  info.class_userdata = (void *)Dart_NewPersistentHandle(type_arg);
  info.create_instance_func = GodotDartBindings::class_create_instance;
  info.free_instance_func = GodotDartBindings::class_free_instance;
  info.get_virtual_func = GodotDartBindings::get_virtual_func;

  GDE->classdb_register_extension_class(GDEWrapper::instance()->lib(), sn_name, sn_parent, &info);
}

void bind_method(Dart_NativeArguments args) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    Dart_ThrowException(Dart_NewStringFromCString("GodotDart has been shutdown!"));
    return;
  }

  Dart_Handle d_bind_type_info = Dart_GetNativeArgument(args, 1);

  const char *method_name = nullptr;
  Dart_StringToCString(Dart_GetNativeArgument(args, 2), &method_name);

  Dart_Handle d_return_type_info = Dart_GetNativeArgument(args, 3);
  Dart_Handle d_argument_list = Dart_GetNativeArgument(args, 4);

  TypeInfo bind_type_info;
  type_info_from_dart(&bind_type_info, d_bind_type_info);

  TypeInfo return_type_info;
  type_info_from_dart(&return_type_info, d_return_type_info);

  intptr_t arg_length = 0;
  Dart_ListLength(d_argument_list, &arg_length);

  std::vector<TypeInfo> argument_list;
  argument_list.reserve(arg_length);
  for (intptr_t i = 0; i < arg_length; ++i) {
    Dart_Handle d_arg = Dart_ListGetAt(d_argument_list, i);
    TypeInfo arg;

    type_info_from_dart(&arg, d_arg);
    argument_list.push_back(arg);
  }

  bindings->bind_method(bind_type_info, method_name, return_type_info, argument_list);
}

void gd_string_to_dart_string(Dart_NativeArguments args) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    Dart_ThrowException(Dart_NewStringFromCString("GodotDart has been shutdown!"));
    return;
  }

  Dart_Handle dart_gd_string = Dart_GetNativeArgument(args, 1);
  GDExtensionConstStringPtr gd_string = get_opaque_address(dart_gd_string);

  char16_t length = GDE->string_to_utf16_chars(gd_string, nullptr, 0);
  char16_t *temp = (char16_t *)_alloca(sizeof(char16_t) * (length + 1));
  GDE->string_to_utf16_chars(gd_string, temp, length);
  temp[length] = 0;

  Dart_Handle dart_string = Dart_NewStringFromUTF16((uint16_t *)temp, length);
  if (Dart_IsError(dart_string)) {
    Dart_ThrowException(Dart_NewStringFromCString(Dart_GetError(dart_string)));
    return;
  }

  Dart_SetReturnValue(args, dart_string);
}

void gd_object_to_dart_object(Dart_NativeArguments args) {
  GodotDartBindings *bindings = GodotDartBindings::instance();
  if (!bindings) {
    Dart_ThrowException(Dart_NewStringFromCString("GodotDart has been shutdown!"));
    return;
  }

  Dart_Handle dart_gd_object = Dart_GetNativeArgument(args, 1);
  Dart_Handle address = Dart_GetField(dart_gd_object, Dart_NewStringFromCString("address"));
  if (Dart_IsError(address)) {
    GD_PRINT_ERROR(Dart_GetError(address));
    Dart_ThrowException(Dart_NewStringFromCString(Dart_GetError(address)));
    return;
  }
  uint64_t object_ptr = 0;
  Dart_IntegerToUint64(address, &object_ptr);

  Dart_Handle dart_bindings_ptr = Dart_GetNativeArgument(args, 2);
  const GDExtensionInstanceBindingCallbacks *bindings_callbacks = &__binding_callbacks;
  if (!Dart_IsNull(dart_bindings_ptr)) {
    address = Dart_GetField(dart_bindings_ptr, Dart_NewStringFromCString("address"));
    if (Dart_IsError(address)) {
      GD_PRINT_ERROR(Dart_GetError(address));
      Dart_ThrowException(Dart_NewStringFromCString(Dart_GetError(address)));
      return;
    }
    uint64_t bindings_ptr = 0;
    Dart_IntegerToUint64(address, &bindings_ptr);
    bindings_callbacks = reinterpret_cast<const GDExtensionInstanceBindingCallbacks *>(bindings_ptr);
  }

  GDEWrapper *gde = GDEWrapper::instance();
  Dart_PersistentHandle dart_persistent = (Dart_PersistentHandle)GDE->object_get_instance_binding(
      reinterpret_cast<GDExtensionObjectPtr>(object_ptr), gde->lib(), bindings_callbacks);
  if (dart_persistent == nullptr) {
    Dart_SetReturnValue(args, Dart_Null());
  } else {
    Dart_Handle obj = Dart_HandleFromPersistent(dart_persistent);
    if (Dart_IsError(obj)) {
      GD_PRINT_ERROR(Dart_GetError(obj));
      Dart_ThrowException(Dart_NewStringFromCString(Dart_GetError(address)));
      return;
    }
    Dart_SetReturnValue(args, obj);
  }
}

void dart_object_post_initialize(Dart_NativeArguments args) {
  Dart_Handle dart_self = Dart_GetNativeArgument(args, 0);
  Dart_Handle d_class_type_info = Dart_GetField(dart_self, Dart_NewStringFromCString("staticTypeInfo"));
  if (Dart_IsError(d_class_type_info)) {
    GD_PRINT_ERROR("GodotDart: Error finding typeInfo on object: ");
    GD_PRINT_ERROR(Dart_GetError(d_class_type_info));
  }

  TypeInfo class_type_info;
  type_info_from_dart(&class_type_info, d_class_type_info);

  Dart_Handle owner = Dart_GetField(dart_self, Dart_NewStringFromCString("nativePtr"));
  if (Dart_IsError(owner)) {
    GD_PRINT_ERROR("GodotDart: Error finding owner member for object: ");
    GD_PRINT_ERROR(Dart_GetError(owner));
  }

  Dart_Handle owner_address = Dart_GetField(owner, Dart_NewStringFromCString("address"));
  if (Dart_IsError(owner_address)) {
    GD_PRINT_ERROR("GodotDart: Error getting address for object: ");
    GD_PRINT_ERROR(Dart_GetError(owner_address));
  }

  Dart_PersistentHandle persistent_handle = Dart_NewPersistentHandle(dart_self);
  GDEWrapper *gde = GDEWrapper::instance();

  uint64_t real_address = 0;
  Dart_IntegerToUint64(owner_address, &real_address);

  GDE->object_set_instance(reinterpret_cast<GDExtensionObjectPtr>(real_address), class_type_info.type_name,
                           reinterpret_cast<GDExtensionClassInstancePtr>(persistent_handle));
  GDE->object_set_instance_binding(reinterpret_cast<GDExtensionObjectPtr>(real_address), gde->lib(), persistent_handle,
                                   &__binding_callbacks);
}

Dart_NativeFunction native_resolver(Dart_Handle name, int num_of_arguments, bool *auto_setup_scope) {
  Dart_EnterScope();

  const char *c_name = nullptr;
  Dart_StringToCString(name, &c_name);

  Dart_NativeFunction ret = nullptr;

  if (0 == strcmp(c_name, "GodotDartNativeBindings::bindMethod")) {
    *auto_setup_scope = true;
    ret = bind_method;
  } else if (0 == strcmp(c_name, "GodotDartNativeBindings::bindClass")) {
    *auto_setup_scope = true;
    ret = bind_class;
  } else if (0 == strcmp(c_name, "GodotDartNativeBindings::gdStringToString")) {
    *auto_setup_scope = true;
    ret = gd_string_to_dart_string;
  } else if (0 == strcmp(c_name, "GodotDartNativeBindings::gdObjectToDartObject")) {
    *auto_setup_scope = true;
    ret = gd_object_to_dart_object;
  } else if (0 == strcmp(c_name, "ExtensionType::postInitialize")) {
    *auto_setup_scope = true;
    ret = dart_object_post_initialize;
  }

  Dart_ExitScope();
  return ret;
}

extern "C" {

GDE_EXPORT void variant_copy(void *dest, void *src, int size) {
  memcpy(dest, src, size);
}
}