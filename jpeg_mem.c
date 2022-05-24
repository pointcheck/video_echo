
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>

#include <jpeglib.h>



/* Called before data is read */ 

METHODDEF(void) init_source (j_decompress_ptr dinfo) { 

	/* nothing to do here, really. I mean. I'm not lazy or something, but... 
	   we're actually through here. */ 
}


/* Called if the decoder wants some bytes that we cannot provide... */ 

METHODDEF(boolean) fill_input_buffer (j_decompress_ptr dinfo) { 

	/* we can't do anything about this. 
	   This might happen if the provided buffer is either invalid with regards 
	   to its content or just a to small bufsize has been given. */

	return FALSE; 
}


/* From IJG docs: "it's not clear that being smart is worth much trouble" So I save myself some trouble by ignoring this bit. */ 

METHODDEF(void) skip_input_data (j_decompress_ptr dinfo, INT32 num_bytes) { 

	/* There might be more data to skip than available in buffer. 
	   This clearly is an error, so screw this mess. */ 

	if ((size_t)num_bytes > dinfo->src->bytes_in_buffer) { 
		dinfo->src->next_input_byte = 0; /* no buffer byte */ 
		dinfo->src->bytes_in_buffer = 0; /* no input left */ 
	} else { 
		dinfo->src->next_input_byte += num_bytes; 
		dinfo->src->bytes_in_buffer -= num_bytes; 
	} 
}


/* Finished with decompression */ 

METHODDEF(void) term_source (j_decompress_ptr dinfo) { 
		/* Again. Absolute laziness. Nothing to do here. Boring. */ 
}

GLOBAL(void) jpeg_memory_src (j_decompress_ptr dinfo, unsigned char* buffer, size_t size) { 
	struct jpeg_source_mgr* src;

	/* first call for this instance - need to setup */ 
	if (dinfo->src == 0) { 
		dinfo->src = (struct jpeg_source_mgr *) (*dinfo->mem->alloc_small) ((j_common_ptr) dinfo, JPOOL_PERMANENT, sizeof (struct jpeg_source_mgr)); 
	}

	src = dinfo->src; 
	src->next_input_byte = buffer; 
	src->bytes_in_buffer = size; 
	src->init_source = init_source; 
	src->fill_input_buffer = fill_input_buffer; 
	src->skip_input_data = skip_input_data; 
	src->term_source = term_source; 

	/* IJG recommend to use their function - as I don't know ** about how to do better, I follow this recommendation */ 
	src->resync_to_restart = jpeg_resync_to_restart; 
	
}


int jpeg_decompress(char *from, int fromLen, char *to, int *width, int *height) {

        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        JSAMPARRAY buffer;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);


	jpeg_memory_src (&cinfo, from, fromLen);

	jpeg_read_header(&cinfo, TRUE);

	jpeg_start_decompress(&cinfo);

	int row_stride = cinfo.output_width * cinfo.output_components;

	*width = cinfo.output_width;
	*height = cinfo.output_height;

	buffer = (*cinfo.mem->alloc_sarray)
		((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, cinfo.output_height);

	int rowsRead = 0;

	while (cinfo.output_scanline < cinfo.output_height) {

		rowsRead += jpeg_read_scanlines(&cinfo, &buffer[rowsRead], cinfo.output_height - rowsRead);
	}


	//rgb_to_framebuffer(&vd, cinfo.output_width, cinfo.output_height, 0, 0, buffer);

	int x, y;
	unsigned int *to_ptr = (unsigned int*) to;

        for(y = 0; y < *height; y++){
                for(x = 0; x < *width; x++) {
                        //*(to_ptr++) = (buffer[y][x * 3] << 16) | (buffer[y][x * 3 + 1] << 8) | (buffer[y][x * 3 + 2]);
                        *(to_ptr++) = 
				(((int)*(buffer++)) << 16) | 
				(((int)*(buffer++)) << 8) | 
				(((int)*(buffer++)));
                }
        }


	jpeg_finish_decompress(&cinfo);

}

