/* Décompresseur deflate (RFC 1951) autonome : évite de dépendre de zlib côté Dreamcast. */
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace Inflate {
/* Décompresse un flux deflate brut. Renvoie le nombre d'octets produits, ou -1 en cas d'erreur
   (données corrompues ou tampon de sortie trop petit). */
long raw(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstCap);
}
