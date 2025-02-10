SECTIONS {
	__patchable_function_entries : { *(__patchable_function_entries) }
	.text : {
		__bpf_trace_amdgpu_device_resume_end.4b507fd3698fffe042f9b404816d9613.cfi_jt = . + 0x1c2a44;
	}
}
