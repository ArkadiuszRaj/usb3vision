/*
 * u3v_example.c - Example userspace application for USB3 Vision driver
 *
 * Demonstrates:
 *   1. Connecting to a U3V camera via the kernel driver
 *   2. Reading device info from ABRM/SBRM registers
 *   3. Downloading the GenICam XML device description
 *   4. Configuring the stream and acquiring image frames
 *
 * Build:
 *   gcc -Wall -O2 -o u3v_example u3v_example.c -lz
 *
 * Usage:
 *   sudo ./u3v_example /dev/u3v0
 *
 * The XML is typically zlib-compressed inside the camera.
 * Link with -lz (zlib) for decompression. If you don't need
 * XML extraction, remove the zlib parts and the -lz flag.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <zlib.h>

/* Include the driver's shared header for ioctl definitions */
#include "u3v_shared.h"

/*
 * GenCP Manifest Table entry layout (per USB3 Vision / GenCP spec).
 * The manifest table address is stored at ABRM offset 0x1D0.
 * At that address: u64 entry_count, then entry_count entries.
 */
#pragma pack(push, 1)
struct manifest_entry {
	uint64_t file_info;        /* version, type, compression scheme */
	uint64_t register_address; /* address of the XML data in device memory */
	uint64_t file_size;        /* size of the file in bytes */
	uint8_t  sha1_hash[20];
	uint8_t  reserved[36];     /* pad to 72 bytes per entry */
};
#pragma pack(pop)

/* Manifest file_info field bit masks */
#define MANIFEST_FILE_TYPE_MASK       0x0000FF0000000000ULL
#define MANIFEST_FILE_TYPE_SHIFT      40
#define MANIFEST_COMPRESSION_MASK     0x00FF000000000000ULL
#define MANIFEST_COMPRESSION_SHIFT    48

#define FILE_TYPE_DEVICE_XML  0
#define COMPRESSION_NONE      0
#define COMPRESSION_ZIP       1  /* zlib/deflate */

/* SI_CONTROL register bits */
#define SI_CONTROL_ENABLE     0x00000001

/* ------------------------------------------------------------------ */
/* Helper: read device registers via the driver's ioctl               */
/* ------------------------------------------------------------------ */
static int u3v_read(int fd, uint64_t address, void *buf, uint32_t size)
{
	uint32_t bytes_read = 0;
	struct u3v_read_memory req = {
		.address       = address,
		.u_buffer      = buf,
		.transfer_size = size,
		.u_bytes_read  = &bytes_read,
	};

	int ret = ioctl(fd, U3V_IOCTL_READ, &req);
	if (ret != 0) {
		fprintf(stderr, "u3v_read: ioctl failed at addr 0x%llX, size %u: %s\n",
			(unsigned long long)address, size, strerror(errno));
		return -1;
	}
	if (bytes_read != size) {
		fprintf(stderr, "u3v_read: short read at 0x%llX: got %u, expected %u\n",
			(unsigned long long)address, bytes_read, size);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: write device registers via the driver's ioctl              */
/* ------------------------------------------------------------------ */
static int u3v_write(int fd, uint64_t address, const void *buf, uint32_t size)
{
	uint32_t bytes_written = 0;
	struct u3v_write_memory req = {
		.address        = address,
		.u_buffer       = (void *)buf,
		.transfer_size  = size,
		.u_bytes_written = &bytes_written,
	};

	int ret = ioctl(fd, U3V_IOCTL_WRITE, &req);
	if (ret != 0) {
		fprintf(stderr, "u3v_write: ioctl failed at addr 0x%llX: %s\n",
			(unsigned long long)address, strerror(errno));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: read a string register (64-byte field in ABRM)             */
/* ------------------------------------------------------------------ */
static int u3v_read_string(int fd, uint64_t address, char *out, size_t out_sz)
{
	char buf[64];
	memset(buf, 0, sizeof(buf));
	if (u3v_read(fd, address, buf, sizeof(buf)) != 0)
		return -1;
	buf[63] = '\0';
	snprintf(out, out_sz, "%s", buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 1: Read device information from ABRM registers                */
/* ------------------------------------------------------------------ */
static int read_device_info(int fd, uint64_t *sbrm_addr_out,
			    uint64_t *manifest_addr_out)
{
	uint32_t gencp_ver, u3v_ver;
	uint64_t sbrm_addr, manifest_table_addr;
	char manufacturer[64], model[64], serial[64], guid[64];

	printf("=== Device Information (ABRM) ===\n");

	if (u3v_read(fd, ABRM_GENCP_VERSION, &gencp_ver, 4) != 0)
		return -1;
	printf("  GenCP version     : %u.%u\n", gencp_ver >> 16, gencp_ver & 0xFFFF);

	if (u3v_read(fd, ABRM_SBRM_ADDRESS, &sbrm_addr, 8) != 0)
		return -1;

	/* Read U3V version from SBRM */
	if (u3v_read(fd, sbrm_addr + SBRM_U3V_VERSION, &u3v_ver, 4) != 0)
		return -1;
	printf("  U3V version       : %u.%u\n", u3v_ver >> 16, u3v_ver & 0xFFFF);

	u3v_read_string(fd, ABRM_MANUFACTURER_NAME, manufacturer, sizeof(manufacturer));
	u3v_read_string(fd, ABRM_MODEL_NAME, model, sizeof(model));
	u3v_read_string(fd, ABRM_SERIAL_NUMBER, serial, sizeof(serial));

	/* GUID is stored as a string index in the USB descriptor, but the
	 * driver populates it in sysfs. We can also try reading the ABRM
	 * directly if the camera supports it. For sysfs approach, see
	 * /sys/class/usbmisc/u3vX/device/device_guid */
	memset(guid, 0, sizeof(guid));

	printf("  Manufacturer      : %s\n", manufacturer);
	printf("  Model             : %s\n", model);
	printf("  Serial            : %s\n", serial);
	printf("  SBRM address      : 0x%llX\n", (unsigned long long)sbrm_addr);

	if (u3v_read(fd, ABRM_MANIFEST_TABLE_ADDRESS, &manifest_table_addr, 8) != 0)
		return -1;
	printf("  Manifest table    : 0x%llX\n", (unsigned long long)manifest_table_addr);

	*sbrm_addr_out = sbrm_addr;
	*manifest_addr_out = manifest_table_addr;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 2: Download and extract the GenICam XML from device memory     */
/* ------------------------------------------------------------------ */
static int download_xml(int fd, uint64_t manifest_table_addr)
{
	uint64_t entry_count = 0;
	struct manifest_entry entry;
	uint8_t file_type, compression;
	uint8_t *raw_data = NULL;
	int ret = -1;

	printf("\n=== GenICam XML Download ===\n");

	/* Read entry count at the beginning of the manifest table */
	if (u3v_read(fd, manifest_table_addr, &entry_count, 8) != 0)
		return -1;

	printf("  Manifest entries  : %llu\n", (unsigned long long)entry_count);
	if (entry_count == 0 || entry_count > 64) {
		fprintf(stderr, "  Invalid manifest entry count\n");
		return -1;
	}

	/* Read the first entry (typically the device XML) */
	if (u3v_read(fd, manifest_table_addr + 8, &entry, sizeof(entry)) != 0)
		return -1;

	file_type   = (entry.file_info & MANIFEST_FILE_TYPE_MASK) >> MANIFEST_FILE_TYPE_SHIFT;
	compression = (entry.file_info & MANIFEST_COMPRESSION_MASK) >> MANIFEST_COMPRESSION_SHIFT;

	printf("  File type         : %u (%s)\n", file_type,
	       file_type == FILE_TYPE_DEVICE_XML ? "Device XML" : "Unknown");
	printf("  Compression       : %u (%s)\n", compression,
	       compression == COMPRESSION_NONE ? "None" :
	       compression == COMPRESSION_ZIP  ? "ZIP/zlib" : "Unknown");
	printf("  Register address  : 0x%llX\n", (unsigned long long)entry.register_address);
	printf("  File size         : %llu bytes\n", (unsigned long long)entry.file_size);

	if (entry.file_size == 0 || entry.file_size > 16 * 1024 * 1024) {
		fprintf(stderr, "  Invalid file size\n");
		return -1;
	}

	/* Read the raw XML data from device memory */
	raw_data = malloc(entry.file_size);
	if (!raw_data) {
		perror("malloc");
		return -1;
	}

	/*
	 * Read in chunks - the driver's max transfer per ioctl may be limited.
	 * GenCP typically supports up to ~64KB per read command.
	 */
	{
		uint32_t offset = 0;
		uint32_t chunk_size = 512;  /* safe GenCP read size */

		while (offset < entry.file_size) {
			uint32_t remaining = entry.file_size - offset;
			uint32_t to_read = remaining < chunk_size ? remaining : chunk_size;

			if (u3v_read(fd, entry.register_address + offset,
				     raw_data + offset, to_read) != 0) {
				fprintf(stderr, "  Error reading XML at offset %u\n", offset);
				goto out;
			}
			offset += to_read;
		}
	}

	printf("  Downloaded %llu bytes of XML data\n",
	       (unsigned long long)entry.file_size);

	/* Decompress or save directly */
	if (compression == COMPRESSION_ZIP) {
		/* zlib decompression */
		uLongf decompressed_size = entry.file_size * 10;  /* estimate */
		uint8_t *xml_data = malloc(decompressed_size);
		if (!xml_data) {
			perror("malloc for decompression");
			goto out;
		}

		int zret = uncompress(xml_data, &decompressed_size,
				      raw_data, entry.file_size);
		if (zret != Z_OK) {
			fprintf(stderr, "  zlib decompression failed: %d\n", zret);
			free(xml_data);
			goto out;
		}

		printf("  Decompressed to %lu bytes\n", decompressed_size);

		/* Save to file */
		FILE *f = fopen("camera_description.xml", "w");
		if (f) {
			fwrite(xml_data, 1, decompressed_size, f);
			fclose(f);
			printf("  Saved to camera_description.xml\n");
		}

		/* Print first 500 chars as preview */
		printf("\n--- XML Preview (first 500 chars) ---\n");
		printf("%.*s\n", (int)(decompressed_size < 500 ? decompressed_size : 500),
		       xml_data);
		printf("--- end preview ---\n");

		free(xml_data);
	} else {
		/* Uncompressed - save directly */
		FILE *f = fopen("camera_description.xml", "w");
		if (f) {
			fwrite(raw_data, 1, entry.file_size, f);
			fclose(f);
			printf("  Saved to camera_description.xml\n");
		}
	}

	ret = 0;
out:
	free(raw_data);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Step 3: Configure stream, acquire frames                           */
/* ------------------------------------------------------------------ */

/*
 * NOTE: Proper image acquisition requires knowing the camera's
 * register layout from the GenICam XML. The addresses below are
 * placeholders - you MUST parse the XML (or use a GenICam library
 * like Aravis) to find the actual register addresses for your camera.
 *
 * Typical GenICam bootstrap registers that need to be written:
 *   - AcquisitionMode  -> set to 0 (Continuous) or 1 (SingleFrame)
 *   - AcquisitionStart -> write 1 to trigger
 *   - AcquisitionStop  -> write 1 to stop
 *
 * The SIRM (Streaming Interface Register Map) is handled by the
 * kernel driver internally when you call CONFIGURE_STREAM.
 */

struct camera_params {
	uint64_t sirm_addr;
	uint32_t width;
	uint32_t height;
	uint32_t pixel_format;
	uint64_t payload_size;
	/*
	 * These must come from the GenICam XML. They are camera-specific.
	 * Aravis or similar GenICam parsers can provide them.
	 */
	uint64_t acq_mode_addr;
	uint64_t acq_start_addr;
	uint64_t acq_stop_addr;
};

static int read_sirm_info(int fd, uint64_t sbrm_addr, struct camera_params *cam)
{
	uint64_t u3v_capability = 0;

	if (u3v_read(fd, sbrm_addr + SBRM_U3VCP_CAPABILITY, &u3v_capability, 8) != 0)
		return -1;

	if (!(u3v_capability & SIRM_AVAILABLE_MASK)) {
		fprintf(stderr, "SIRM not available on this device\n");
		return -1;
	}

	if (u3v_read(fd, sbrm_addr + SBRM_SIRM_ADDRESS, &cam->sirm_addr, 8) != 0)
		return -1;

	printf("\n=== Stream Interface (SIRM) ===\n");
	printf("  SIRM address      : 0x%llX\n", (unsigned long long)cam->sirm_addr);

	/* Read the requested payload size from the camera */
	if (u3v_read(fd, cam->sirm_addr + SI_REQ_PAYLOAD_SIZE, &cam->payload_size, 8) != 0)
		return -1;

	printf("  Req payload size  : %llu\n", (unsigned long long)cam->payload_size);
	return 0;
}

static int acquire_frames(int fd, struct camera_params *cam, int num_frames)
{
	uint32_t max_leader_size = 0;
	uint32_t max_trailer_size = 0;
	uint32_t alignment = 0;
	int ret;

	printf("\n=== Frame Acquisition ===\n");

	/* 1. Get stream alignment requirement */
	{
		struct u3v_get_stream_alignment align_req = {
			.u_stream_alignment = &alignment,
		};
		ret = ioctl(fd, U3V_IOCTL_GET_STREAM_ALIGNMENT, &align_req);
		if (ret != 0) {
			fprintf(stderr, "GET_STREAM_ALIGNMENT failed: %s\n", strerror(errno));
			return -1;
		}
		printf("  Stream alignment  : %u bytes\n", alignment);
	}

	/*
	 * 2. Configure the stream interface.
	 *    image_buffer_size = payload size from camera (width * height * bpp typically).
	 *    max_urb_size = maximum USB transfer size per URB. 2MB is a common choice.
	 */
	uint64_t image_size = cam->payload_size;
	if (image_size == 0) {
		fprintf(stderr, "Payload size is 0 - camera may need configuration first\n");
		return -1;
	}

	{
		struct u3v_configure_stream config = {
			.image_buffer_size      = image_size,
			.chunk_data_buffer_size = 0,
			.max_urb_size           = 2 * 1024 * 1024,  /* 2 MB */
			.u_max_leader_size      = &max_leader_size,
			.u_max_trailer_size     = &max_trailer_size,
		};
		ret = ioctl(fd, U3V_IOCTL_CONFIGURE_STREAM, &config);
		if (ret != 0) {
			fprintf(stderr, "CONFIGURE_STREAM failed: %s (ret=%d)\n",
				strerror(errno), ret);
			return -1;
		}
		printf("  Max leader size   : %u\n", max_leader_size);
		printf("  Max trailer size  : %u\n", max_trailer_size);
	}

	/* 3. Allocate image buffer (page-aligned for DMA) */
	size_t alloc_size = (image_size + 4095) & ~4095ULL;
	void *image_buffer = NULL;
	ret = posix_memalign(&image_buffer, 4096, alloc_size);
	if (ret != 0 || !image_buffer) {
		fprintf(stderr, "Failed to allocate image buffer (%zu bytes)\n", alloc_size);
		goto unconfigure_stream;
	}
	memset(image_buffer, 0, alloc_size);

	/* Leader and trailer buffers */
	void *leader_buf  = calloc(1, max_leader_size);
	void *trailer_buf = calloc(1, max_trailer_size);
	if (!leader_buf || !trailer_buf) {
		fprintf(stderr, "Failed to allocate leader/trailer buffers\n");
		goto cleanup_buffers;
	}

	/* 4. Configure a buffer with the driver */
	uint64_t buffer_handle = 0;
	{
		struct u3v_configure_buffer config_buf = {
			.u_image_buffer      = image_buffer,
			.u_chunk_data_buffer = NULL,
			.u_buffer_handle     = &buffer_handle,
		};
		ret = ioctl(fd, U3V_IOCTL_CONFIGURE_BUFFER, &config_buf);
		if (ret != 0) {
			fprintf(stderr, "CONFIGURE_BUFFER failed: %s\n", strerror(errno));
			goto cleanup_buffers;
		}
		printf("  Buffer handle     : %llu\n", (unsigned long long)buffer_handle);
	}

	/*
	 * 5. Enable streaming on the camera side.
	 *    Write SI_CONTROL to enable the SIRM, then send
	 *    the AcquisitionStart command.
	 *
	 *    WARNING: acq_start_addr / acq_stop_addr MUST come from parsing
	 *    the GenICam XML. The values below are PLACEHOLDERS.
	 */
	{
		uint32_t si_enable = SI_CONTROL_ENABLE;
		if (u3v_write(fd, cam->sirm_addr + SI_CONTROL, &si_enable, 4) != 0) {
			fprintf(stderr, "Failed to enable SIRM\n");
			goto unconfigure_buffer;
		}
		printf("  SIRM enabled\n");
	}

	if (cam->acq_start_addr != 0) {
		uint32_t acq_mode = 0;  /* 0 = Continuous */
		u3v_write(fd, cam->acq_mode_addr, &acq_mode, 4);

		uint32_t acq_start = 1;
		if (u3v_write(fd, cam->acq_start_addr, &acq_start, 4) != 0) {
			fprintf(stderr, "Failed to start acquisition\n");
			goto stop_stream;
		}
		printf("  Acquisition started\n");
	} else {
		printf("  WARNING: AcquisitionStart address not set!\n");
		printf("  You must parse the GenICam XML to find it.\n");
		printf("  Attempting to receive frames anyway...\n");
	}

	/* 6. Queue buffer, wait, process - repeat for each frame */
	for (int frame = 0; frame < num_frames; frame++) {
		struct buffer_complete_data bcd;
		uint32_t leader_size = max_leader_size;
		uint32_t trailer_size = max_trailer_size;

		/* Queue the buffer to the camera */
		{
			struct u3v_queue_buffer queue = {
				.buffer_handle = buffer_handle,
			};
			ret = ioctl(fd, U3V_IOCTL_QUEUE_BUFFER, &queue);
			if (ret != 0) {
				fprintf(stderr, "QUEUE_BUFFER failed: %s\n", strerror(errno));
				break;
			}
		}

		/* Wait for the buffer to be filled with image data */
		{
			struct u3v_wait_for_buffer wait = {
				.buffer_handle          = buffer_handle,
				.u_leader_buffer        = leader_buf,
				.u_leader_size          = &leader_size,
				.u_trailer_buffer       = trailer_buf,
				.u_trailer_size         = &trailer_size,
				.u_buffer_complete_data = &bcd,
			};
			ret = ioctl(fd, U3V_IOCTL_WAIT_FOR_BUFFER, &wait);
			if (ret != 0) {
				fprintf(stderr, "WAIT_FOR_BUFFER failed: %s\n", strerror(errno));
				break;
			}
		}

		/* Parse leader to get frame metadata */
		struct leader_header *leader = (struct leader_header *)leader_buf;
		struct trailer_header *trailer = (struct trailer_header *)trailer_buf;

		printf("\n  Frame %d:\n", frame + 1);
		printf("    Block ID          : %llu\n",
		       (unsigned long long)leader->block_id);
		printf("    Payload type      : 0x%04X\n", leader->payload_type);
		printf("    Leader size       : %u bytes\n", leader_size);
		printf("    Trailer size      : %u bytes\n", trailer_size);
		printf("    Payload received  : %llu bytes\n",
		       (unsigned long long)bcd.payload_bytes_received);
		printf("    Valid payload     : %llu bytes\n",
		       (unsigned long long)trailer->valid_payload_size);
		printf("    Status            : %d\n", bcd.status);
		printf("    Incomplete URBs   : %u / %u\n",
		       bcd.incomplete_urb_count, bcd.expected_urb_count);

		/* Check if we got image data */
		if (leader->payload_type == U3V_IMAGE ||
		    leader->payload_type == U3V_IMAGE_EXTENDED_CHUNK) {
			/*
			 * image_buffer now contains raw pixel data.
			 * Parse image_leader_info for dimensions:
			 */
			if (leader_size >= sizeof(struct leader_header) +
					   sizeof(struct image_leader_info)) {
				struct image_leader_info *img =
					(struct image_leader_info *)
					((uint8_t *)leader_buf + sizeof(struct leader_header));
				printf("    Image: %ux%u, pixel format 0x%08X\n",
				       img->size_x, img->size_y, img->pixel_format);
				printf("    Offset: (%u, %u), padding_x: %u\n",
				       img->offset_x, img->offset_y, img->padding_x);
			}

			/*
			 * Here you would save or process image_buffer.
			 * Example: save raw frame to a file.
			 */
			if (frame == 0) {
				char filename[64];
				snprintf(filename, sizeof(filename), "frame_%04d.raw", frame);
				FILE *f = fopen(filename, "wb");
				if (f) {
					fwrite(image_buffer, 1,
					       bcd.payload_bytes_received, f);
					fclose(f);
					printf("    Saved to %s\n", filename);
				}
			}
		}
	}

	/* 7. Stop acquisition and clean up */
stop_stream:
	if (cam->acq_stop_addr != 0) {
		uint32_t acq_stop = 1;
		u3v_write(fd, cam->acq_stop_addr, &acq_stop, 4);
		printf("\n  Acquisition stopped\n");
	}

	{
		uint32_t si_disable = 0;
		u3v_write(fd, cam->sirm_addr + SI_CONTROL, &si_disable, 4);
	}

	/* Cancel any in-flight buffers */
	ioctl(fd, U3V_IOCTL_CANCEL_ALL_BUFFERS, NULL);

unconfigure_buffer:
	{
		struct u3v_unconfigure_buffer unconfig = {
			.buffer_handle = buffer_handle,
		};
		ioctl(fd, U3V_IOCTL_UNCONFIGURE_BUFFER, &unconfig);
	}

cleanup_buffers:
	free(leader_buf);
	free(trailer_buf);
	free(image_buffer);

unconfigure_stream:
	ioctl(fd, U3V_IOCTL_UNCONFIGURE_STREAM, NULL);

	return ret;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
	const char *dev_path = "/dev/u3v0";
	uint64_t sbrm_addr = 0;
	uint64_t manifest_addr = 0;
	struct camera_params cam;
	int fd, ret = 1;

	if (argc > 1)
		dev_path = argv[1];

	printf("USB3 Vision Example Application\n");
	printf("Opening device: %s\n\n", dev_path);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		fprintf(stderr, "Hint: check permissions (try sudo) and that a U3V camera is connected\n");
		return 1;
	}

	/* --- Phase 1: Read device information --- */
	if (read_device_info(fd, &sbrm_addr, &manifest_addr) != 0) {
		fprintf(stderr, "\nFailed to read device info. Is this a valid U3V camera?\n");
		goto done;
	}

	/* --- Phase 2: Download GenICam XML --- */
	if (download_xml(fd, manifest_addr) != 0) {
		fprintf(stderr, "\nFailed to download XML (non-fatal, continuing...)\n");
	}

	/* --- Phase 3: Read SIRM and acquire frames --- */
	memset(&cam, 0, sizeof(cam));

	if (read_sirm_info(fd, sbrm_addr, &cam) != 0) {
		fprintf(stderr, "\nStream interface not available\n");
		goto done;
	}

	/*
	 * TODO: Parse camera_description.xml (the GenICam XML you downloaded
	 * in Phase 2) to find the actual register addresses for:
	 *   - AcquisitionMode
	 *   - AcquisitionStart
	 *   - AcquisitionStop
	 *   - Width, Height, PixelFormat
	 *
	 * These are camera-specific and encoded in the XML's <IntReg>,
	 * <Command>, and <Integer> nodes.
	 *
	 * Libraries like Aravis (https://github.com/AravisProject/aravis)
	 * provide full GenICam XML parsing. For a minimal approach, you
	 * could grep the XML for the register addresses.
	 *
	 * For now, set them to 0 which skips the AcquisitionStart write.
	 * The camera won't send frames without it, but the stream
	 * infrastructure will be set up correctly.
	 */
	cam.acq_mode_addr  = 0;  /* REPLACE with real address from XML */
	cam.acq_start_addr = 0;  /* REPLACE with real address from XML */
	cam.acq_stop_addr  = 0;  /* REPLACE with real address from XML */

	printf("\n  NOTE: AcquisitionStart/Stop addresses must be obtained\n");
	printf("  from the GenICam XML. See camera_description.xml.\n");

	if (acquire_frames(fd, &cam, 5) != 0) {
		fprintf(stderr, "\nFrame acquisition failed\n");
		goto done;
	}

	ret = 0;
	printf("\nDone.\n");

done:
	close(fd);
	return ret;
}
