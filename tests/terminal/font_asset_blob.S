/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Embed the exact generated production PSF2 asset for the host font fixture. */
#ifdef __APPLE__
.section __DATA,__data
.balign 4
.globl __binary_kernel_assets_font_psf_start
__binary_kernel_assets_font_psf_start:
.incbin "kernel/assets/font.psf"
.globl __binary_kernel_assets_font_psf_end
__binary_kernel_assets_font_psf_end:
#else
.section .data
.balign 4
.globl _binary_kernel_assets_font_psf_start
_binary_kernel_assets_font_psf_start:
.incbin "kernel/assets/font.psf"
.globl _binary_kernel_assets_font_psf_end
_binary_kernel_assets_font_psf_end:
.section .note.GNU-stack,"",@progbits
#endif
